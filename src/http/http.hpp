// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/frame_type.hpp"
#include "config.hpp"
#include "contract/thread_safety.hpp"
#include "types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string, std::less<>> query;
    std::map<std::string, std::string, std::less<>> headers;
    Bytes body;
    // Set once the bearer token validates against the cluster session store.
    // Absent for exempt routes (session creation, signed capability URLs).
    std::optional<SessionIdentity> session;
    // Set when re-run after a deferral (see HttpDeferral): the parked state and
    // its deadline, so the handler can tell arrival from timeout statelessly.
    bool resumed{};
    std::shared_ptr<void> resumed_state{};
    Clock::time_point resume_deadline{};
    // The request's work class, from the route table; the work it does
    // carries it (frame_for(work) for its DATA work).
    WorkClass work{WorkClass::viewer};
    // Every value of each repeatable query parameter, in order; `query` keeps
    // the last.
    std::map<std::string, std::vector<std::string>, std::less<>> query_all{};
};

class HttpBodySource {
  public:
    virtual ~HttpBodySource() = default;
    virtual uint64_t size() const = 0;
    // Runs on a compute-pool thread, never the reactor: it may block.
    virtual size_t read(uint64_t offset, std::span<uint8_t> destination) = 0;
    // The whole body [0, size()) when it stays in memory for this source's life;
    // the reactor sends straight from it with no copy or pool hop. Null means use
    // read().
    virtual const uint8_t* resident() const noexcept {
        return nullptr;
    }
};

// One-shot wake for a deferred request: the producer calls fire(), the server
// arm(). Either order works, so a racing producer cannot lose the wakeup.
class HttpWaker {
    Mutex mutex_;
    bool fired_ MACHA_GUARDED_BY(mutex_){};
    std::function<void()> wake_ MACHA_GUARDED_BY(mutex_);

  public:
    void fire();
    void arm(std::function<void()> wake);
};

// Returned instead of blocking: wake on this, give up at that deadline, hand
// back this state. The connection parks threadless and the handler re-runs
// with HttpRequest::resumed set.
struct HttpDeferral {
    std::shared_ptr<HttpWaker> waker{};
    Clock::time_point deadline{};
    std::shared_ptr<void> state{};
};

struct HttpResponse {
    int status{200};
    std::string content_type{"application/json; charset=utf-8"};
    std::map<std::string, std::string, std::less<>> headers;
    Bytes body;
    std::shared_ptr<HttpBodySource> stream;
    std::optional<HttpDeferral> defer;

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
// Stamp a snake_case top-level `status` on every JSON object response: a
// handler's own is kept, else "ok" for success and "error" for errors.
// Streams, deferrals, 204/304 and non-object bodies are untouched. Applied once
// on the lane worker, before compression.
void http_stamp_status(HttpResponse&);
// Single linear scan that builds nothing: catalogue bodies reach megabytes.
bool http_json_object_has_key(std::string_view json, std::string_view key);
HttpResponse http_error(int status, std::string_view code, std::string_view message);
// Plus a machine-readable reason the client can act on.
HttpResponse http_error(int status, std::string_view code, std::string_view message,
                        std::string_view reason);

// What a failure is a property of, so a client can decide whether another
// node is worth trying:
//   content  the bytes; every node holds the same bytes. Do not walk.
//   node     this node's view or capability (missing extent, slow mount,
//            build without an encoder). Walk.
//   request  the request is wrong; every node refuses it. Fix the request.
//   cluster  cluster-wide; every node answers the same. Do not walk.
enum class FailureScope { content, node, request, cluster };
const char* failure_scope_name(FailureScope) noexcept;

// The axes a client needs from a failure. An omitted field means "not said",
// never a default: an unintended `false` is worse than a known gap.
struct FailureAxes {
    std::optional<FailureScope> scope;
    // Whether the node is fit for other work. A per-title fault leaves it healthy,
    // so a 5xx with node_healthy true is consistent.
    std::optional<bool> node_healthy;
    // Whether a different instruction could succeed on this node: permission,
    // not instruction. "Same instruction, wait" is 429 with Retry-After instead.
    std::optional<bool> alternative_may_succeed;
};

// With the axes attached. An empty `reason` is omitted.
HttpResponse http_error(int status, std::string_view code, std::string_view message,
                        std::string_view reason, const FailureAxes& axes);
std::string http_url_decode(std::string_view value);

// Runs a handler as HttpServer does, waiting out deferrals on the caller's
// thread and re-running until it answers.
HttpResponse http_resolve(const std::function<HttpResponse(const HttpRequest&)>& handler,
                          HttpRequest request);

struct HttpServerDiagnostics {
    struct Lane {
        uint64_t workers{};
        uint64_t busy{};
        uint64_t queued{};
        uint64_t peak_queued{};
        uint64_t queue_wait_ms_max{};
        uint64_t handled{};
    };
    uint64_t reactor_passes{};
    uint64_t reactor_stalls{};
    uint64_t reactor_longest_pass_ms{};
    uint64_t connections_open{};
    uint64_t connections_idle_keep_alive{};
    uint64_t connections_writing{};
    uint64_t connections_deferred{};
    uint64_t connections_refused{};
    uint64_t staged_bytes{};
    uint64_t requests_served{};
    uint64_t requests_deferred{};
    uint64_t requests_overloaded{};
    uint64_t slow_requests{};
    // Gzipped responses, and bytes saved on the wire.
    uint64_t responses_compressed{};
    uint64_t compression_bytes_saved{};
    Lane control{};
    Lane data{};
};

// One reactor thread owns every socket and waits only in poll(); it never
// calls anything that sleeps. A bounded two-lane compute pool runs handlers
// and body reads; a handler waiting on the media pipeline defers.
class HttpServer {
  public:
    using SessionAuthenticator = std::function<std::optional<SessionIdentity>(std::string_view)>;

    HttpServer(CatalogueApiConfig, std::function<HttpResponse(const HttpRequest&)>,
               std::function<bool(const HttpRequest&)> bearer_exempt = {},
               SessionAuthenticator authenticate = {});
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Each request's work class, from the service's route table. Control
    // requests run on the control pool, every other class on the data pool.
    // Cheap and pure: the reactor calls it. Unset, every request is
    // viewer-class. Set before start().
    void set_work_class(std::function<WorkClass(std::string_view path)> classify);
    // Called with a request's class as it is served (not when it is refused
    // for want of a session), and again with the bytes of every send while
    // its response drains: served traffic is work of that class for as long
    // as it flows, so a body draining to a slow client keeps a viewer
    // present. Set before start().
    void set_activity(std::function<void(WorkClass, uint64_t bytes)> note);
    // A GET or HEAD of exactly `path` is answered by the reactor itself, never
    // queued for a worker, so nothing on either lane can delay it. Only for a
    // handler that reads atomics and builds a small body. Set before start().
    void set_inline_route(std::string path, std::function<HttpResponse()> handler);
    // Test-only: called once per reactor pass, inside the measured region.
    void set_reactor_pass_hook(std::function<void()> hook);

    void start();
    void request_stop();
    void stop();
    bool running() const;
    uint16_t bound_port() const;
    HttpServerDiagnostics diagnostics() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace macha

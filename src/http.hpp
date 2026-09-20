// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
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
    // Set by HttpServer once the bearer token has been validated against the
    // cluster session store. Absent for exempt routes (session creation,
    // signed streaming/asset capability URLs).
    std::optional<SessionIdentity> session;
    // Set when the request is being run again after the handler deferred it
    // (see HttpDeferral): the state the handler parked, and the deadline it
    // named, so it can tell "woken because the thing arrived" from "woken
    // because time ran out" without keeping any state of its own.
    bool resumed{};
    std::shared_ptr<void> resumed_state{};
    Clock::time_point resume_deadline{};
};

class HttpBodySource {
  public:
    virtual ~HttpBodySource() = default;
    virtual uint64_t size() const = 0;
    // Runs on a compute-pool thread, never on the reactor: it may block on a
    // disk or a remote replica.
    virtual size_t read(uint64_t offset, std::span<uint8_t> destination) = 0;
    // The whole body [0, size()), when it is already in memory for the life
    // of this source. The reactor sends straight from it, with no copy and
    // no pool hop, which is the hot path for a transcoded fragment. Null
    // means read() has to be asked.
    virtual const uint8_t* resident() const noexcept {
        return nullptr;
    }
};

// A one-shot wake for a deferred request. Whoever produces the awaited thing
// calls fire(); the server calls arm() with what to do when that happens.
// Either order works: firing before arming wakes the request the moment it
// is armed, so a producer racing the handler cannot lose the wakeup.
class HttpWaker {
    std::mutex mutex_;
    bool fired_{};
    std::function<void()> wake_;

  public:
    void fire();
    void arm(std::function<void()> wake);
};

// What a handler returns instead of blocking: "not yet, wake me on this,
// give up at that time, and hand me this back". The server parks the
// connection, costs no thread for the wait, and re-runs the handler with
// HttpRequest::resumed set when the waker fires or the deadline passes.
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
HttpResponse http_error(int status, std::string_view code, std::string_view message);
// Same, plus a machine-readable reason the client can act on. Used where the
// server states why it could not answer and leaves the decision to the client.
HttpResponse http_error(int status, std::string_view code, std::string_view message,
                        std::string_view reason);

// What a failure is a property of. A client recovering from one has to decide
// whether another node is worth trying, and the answer follows from this
// without it needing a table of our error codes:
//
//   content  the bytes are the problem, and every node holds the same bytes.
//            Asking a neighbour spends the viewer's time to be told the same
//            thing. Do not walk.
//   node     this node's view or capability. Another node may well differ --
//            a missing extent, a slow mount, a build without an encoder.
//            Walk.
//   request  the request itself is wrong, and every node would refuse it
//            identically. Walk collects N copies of the caller's own bug, so
//            do not; fix the request.
//
// The third value is not decoration. Without it a malformed request has to be
// labelled content (which says a client bug is a property of the viewer's
// film) or node (which sends the client round the whole cluster collecting
// its own mistake).
enum class FailureScope { content, node, request };
const char* failure_scope_name(FailureScope) noexcept;

// The axes a client needs from a failure, stated rather than inferred.
//
// Every field is optional and an omitted one means "this server does not say",
// never a default -- the same rule as the per-node playback budgets, where
// absence means the node cannot answer and a client must keep its own
// fallback. A `false` we did not mean is worse than a gap the client knows to
// handle, so say nothing rather than guess.
struct FailureAxes {
    std::optional<FailureScope> scope;
    // Whether this says anything about the node's fitness for other work.
    // A per-title fault leaves the node perfectly healthy, so a 5xx with
    // node_healthy true is honest rather than contradictory: one title failed,
    // the node did not. Charging a node's health for a per-title fault takes a
    // working node out of rotation for every other title.
    std::optional<bool> node_healthy;
    // Whether a DIFFERENT instruction could succeed on this same node. It is
    // permission, not instruction: the client decides whether it has anything
    // to give up. "Same instruction, just wait" is not this -- that is 429
    // with Retry-After.
    std::optional<bool> alternative_may_succeed;
};

// As above, with the axes attached. `reason` may be empty to omit it.
HttpResponse http_error(int status, std::string_view code, std::string_view message,
                        std::string_view reason, const FailureAxes& axes);
std::string http_url_decode(std::string_view value);

// Runs a handler the way HttpServer does, deferrals included: a handler that
// defers is waited for (on the caller's thread) and re-run, until it answers.
// For callers that drive a handler directly rather than over a socket.
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
    // Bodies gzipped on the way out, and the bytes that saved on the wire.
    uint64_t responses_compressed{};
    uint64_t compression_bytes_saved{};
    Lane control{};
    Lane data{};
};

// One reactor thread owns every socket and never waits on anything but
// poll(); a bounded compute pool in two lanes runs handlers and body reads;
// a handler that has to wait on the media pipeline defers instead of
// blocking. See TODO/2026-09-15-http-server-reactor-plan.md for why each
// part is where it is, and in particular for the rule the reactor lives by:
// it may not call anything that sleeps.
class HttpServer {
  public:
    using SessionAuthenticator = std::function<std::optional<SessionIdentity>(std::string_view)>;

    HttpServer(CatalogueApiConfig, std::function<HttpResponse(const HttpRequest&)>,
               std::function<bool(const HttpRequest&)> bearer_exempt = {},
               SessionAuthenticator authenticate = {});
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Paths served on the control lane rather than the data lane: a path is
    // a control route when it equals a prefix or begins with it plus '/'.
    // Pure string matching, so the reactor can decide it without calling
    // into the application. Set before start().
    void set_control_prefixes(std::vector<std::string> prefixes);
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

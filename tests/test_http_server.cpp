// SPDX-License-Identifier: GPL-3.0-or-later
// The reactor's contract, over real sockets. Every case here is a property
// TODO/2026-09-15-http-server-reactor-plan.md promised: that nothing one
// connection does can stall another, that control traffic never queues
// behind data traffic, that a wait costs no thread, and that the reactor
// rule -- it may not call anything that sleeps -- is checked at runtime.
#include "test_backend_support.hpp"

#include <sys/resource.h>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

// A body whose bytes are already in memory: the reactor sends straight from
// it, the path a transcoded fragment takes.
class ResidentBody final : public HttpBodySource {
    std::shared_ptr<const Bytes> bytes_;
    std::shared_ptr<std::atomic_bool> destroyed_;

  public:
    explicit ResidentBody(size_t size, std::shared_ptr<std::atomic_bool> destroyed = {})
        : bytes_(std::make_shared<const Bytes>(pattern(size))), destroyed_(std::move(destroyed)) {}
    ~ResidentBody() override {
        if (destroyed_)
            destroyed_->store(true);
    }
    uint64_t size() const override {
        return bytes_->size();
    }
    size_t read(uint64_t offset, std::span<uint8_t> destination) override {
        if (offset >= bytes_->size())
            return 0;
        const auto n =
            static_cast<size_t>(std::min<uint64_t>(destination.size(), bytes_->size() - offset));
        std::copy_n(bytes_->data() + offset, n, destination.data());
        return n;
    }
    const uint8_t* resident() const noexcept override {
        return bytes_->data();
    }
};

// A body the reactor has to ask the pool for, chunk by chunk: the path a
// spilled fragment, a web asset or a direct-play range takes.
class PoolBody final : public HttpBodySource {
    uint64_t size_;

  public:
    explicit PoolBody(uint64_t size) : size_(size) {}
    uint64_t size() const override {
        return size_;
    }
    size_t read(uint64_t offset, std::span<uint8_t> destination) override {
        if (offset >= size_)
            return 0;
        const auto n = static_cast<size_t>(std::min<uint64_t>(destination.size(), size_ - offset));
        for (size_t i = 0; i < n; ++i)
            destination[i] = static_cast<uint8_t>((offset + i) & 0xff);
        return n;
    }
};

CatalogueApiConfig loopback_config() {
    CatalogueApiConfig config;
    config.enabled = true;
    config.listen = "127.0.0.1";
    config.port = 0;
    config.workers = 2;
    config.control_workers = 1;
    config.max_connections = 512;
    config.stream_chunk_bytes = 64 * 1024;
    return config;
}

std::string body_of(const std::string& response) {
    const auto at = response.find("\r\n\r\n");
    return at == std::string::npos ? std::string() : response.substr(at + 4);
}

// Sends a request and leaves the socket alone: nothing is read from it, so
// the server's sends back up against this client's receive window.
int connect_and_stall(uint16_t port, std::string_view path) {
    const int fd = connect_idle(port);
    raw_http_send(fd, "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
    return fd;
}

MACHA_TEST("http_server", test_a_blocking_body_read_on_one_connection_does_not_delay_another) {
    auto config = loopback_config();
    std::atomic_bool entered{};
    TestGate gate;
    HttpServer server(config, [&](const HttpRequest& request) {
        if (request.path == "/stuck") {
            HttpResponse response;
            response.content_type = "application/octet-stream";
            response.stream = std::make_shared<BlockingHttpBody>(entered, gate, 256 * 1024);
            return response;
        }
        if (request.path == "/api/v1/health")
            return http_json(200, "{\"status\":\"ok\"}");
        return http_error(404, "not_found", "not found");
    });
    server.set_control_prefixes({"/api/v1/health"});
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() != 0; }, 1s));

    std::string stuck_response;
    std::jthread stuck([&] { stuck_response = raw_http_get(server.bound_port(), "/stuck"); });
    REQUIRE(wait_until([&] { return entered.load(); }, 2s));

    // The read is parked on a data-lane worker. Health is on the control
    // lane and the reactor never called the body source at all.
    const auto started = Clock::now();
    const auto health = raw_http_get(server.bound_port(), "/api/v1/health");
    CHECK(Clock::now() - started < 500ms);
    CHECK(health.find("HTTP/1.1 200") != std::string::npos);
    CHECK(health.find("X-Robots-Tag: noindex, nofollow") != std::string::npos);

    gate.open();
    stuck.join();
    CHECK(stuck_response.find("HTTP/1.1 200") != std::string::npos);
    CHECK(body_of(stuck_response).size() == 256 * 1024);
    server.stop();
}

MACHA_TEST("http_server",
           test_blocked_data_lane_leaves_control_lane_answering_and_refuses_beyond_its_queue) {
    auto config = loopback_config();
    config.workers = 1;
    config.max_queued_requests = 1;
    TestGate gate;
    HttpServer server(config, [&](const HttpRequest& request) {
        if (request.path == "/block") {
            gate.enter_and_wait();
            return http_json(200, "{\"blocked\":true}");
        }
        if (request.path == "/api/v1/health")
            return http_json(200, "{\"status\":\"ok\"}");
        return http_error(404, "not_found", "not found");
    });
    server.set_control_prefixes({"/api/v1/health"});
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() != 0; }, 1s));

    // One request occupies the only data worker; a second waits in the
    // lane's single queue slot.
    std::string first_response, second_response;
    std::jthread first([&] { first_response = raw_http_get(server.bound_port(), "/block"); });
    REQUIRE(gate.wait_for_entries(1, 2s));
    std::jthread second([&] { second_response = raw_http_get(server.bound_port(), "/block"); });
    REQUIRE(wait_until([&] { return server.diagnostics().data.queued == 1; }, 2s));

    // A third is refused at once rather than queued without bound.
    const auto refused_at = Clock::now();
    const auto third = raw_http_get(server.bound_port(), "/block");
    CHECK(Clock::now() - refused_at < 500ms);
    CHECK(third.find("HTTP/1.1 503") != std::string::npos);
    CHECK(third.find("\"overloaded\"") != std::string::npos);
    // Identified even when refused: a client that confirms an endpoint by
    // its body must not mistake a busy node for a non-Macha host.
    CHECK(third.find("\"service\":\"macha\"") != std::string::npos);
    CHECK(third.find("\"status\":\"busy\"") != std::string::npos);
    CHECK(third.find("Retry-After: 1") != std::string::npos);

    // And control traffic never noticed.
    const auto health_at = Clock::now();
    const auto health = raw_http_get(server.bound_port(), "/api/v1/health");
    CHECK(Clock::now() - health_at < 500ms);
    CHECK(health.find("HTTP/1.1 200") != std::string::npos);

    gate.open();
    first.join();
    second.join();
    CHECK(first_response.find("HTTP/1.1 200") != std::string::npos);
    CHECK(second_response.find("HTTP/1.1 200") != std::string::npos);
    CHECK(server.diagnostics().requests_overloaded == 1);
    server.stop();
}

MACHA_TEST("http_server", test_a_client_that_stops_reading_does_not_delay_another_clients_body) {
    auto config = loopback_config();
    config.workers = 1; // one pool thread, so the stalled body cannot be "on another worker"
    constexpr uint64_t body_size = 4 * 1024 * 1024;
    HttpServer server(config, [&](const HttpRequest& request) {
        HttpResponse response;
        response.content_type = "application/octet-stream";
        if (request.path == "/pool")
            response.stream = std::make_shared<PoolBody>(body_size);
        else if (request.path == "/resident")
            response.stream = std::make_shared<ResidentBody>(body_size);
        else
            return http_error(404, "not_found", "not found");
        return response;
    });
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() != 0; }, 1s));

    // Two clients that never read: their receive windows fill, the kernel
    // stops taking bytes, and the reactor stops asking for chunks on their
    // behalf. Neither the reactor nor the single data worker is occupied
    // by them once the staging window is full.
    const int stalled_pool = connect_and_stall(server.bound_port(), "/pool");
    const int stalled_resident = connect_and_stall(server.bound_port(), "/resident");
    REQUIRE(wait_until([&] { return server.diagnostics().connections_writing == 2; }, 2s));

    for (const char* path : {"/pool", "/resident"}) {
        const auto started = Clock::now();
        const auto response = raw_http_get(server.bound_port(), path);
        const auto elapsed = Clock::now() - started;
        CHECK(response.find("HTTP/1.1 200") != std::string::npos);
        CHECK(body_of(response).size() == body_size);
        CHECK(elapsed < 5s);
    }

    // The stalled connections held at most a staging window each.
    CHECK(server.diagnostics().staged_bytes <=
          2 * config.staging_chunks * config.stream_chunk_bytes);

    ::close(stalled_pool);
    ::close(stalled_resident);
    server.stop();
}

MACHA_TEST("http_server", test_hundreds_of_idle_keep_alive_connections_cost_nothing_but_fds) {
    // The fd budget is the process's: raise the soft limit to what the hard
    // limit allows, and size the case to what is left.
    rlimit limit{};
    REQUIRE(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    if (limit.rlim_cur < 1024 && (limit.rlim_max == RLIM_INFINITY || limit.rlim_max >= 1024)) {
        limit.rlim_cur = 1024;
        (void)setrlimit(RLIMIT_NOFILE, &limit);
        REQUIRE(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    }
    const size_t connections = limit.rlim_cur >= 1024 ? 200 : 32;

    auto config = loopback_config();
    config.workers = 1;
    config.max_connections = connections + 16;
    config.keep_alive_idle_timeout = 30s;
    HttpServer server(config, [&](const HttpRequest& request) {
        if (request.path == "/api/v1/health")
            return http_json(200, "{\"status\":\"ok\"}");
        return http_json(200, "{\"ok\":true}");
    });
    server.set_control_prefixes({"/api/v1/health"});
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() != 0; }, 1s));

    std::vector<int> fds;
    fds.reserve(connections);
    const std::string request = "GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n";
    for (size_t i = 0; i < connections; ++i) {
        const int fd = connect_idle(server.bound_port());
        timeval timeout{5, 0};
        REQUIRE(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        auto response = raw_http_exchange(fd, request);
        REQUIRE(response.status == 200);
        REQUIRE(response.headers["connection"] == "keep-alive");
        fds.push_back(fd);
    }
    REQUIRE(wait_until(
        [&] { return server.diagnostics().connections_idle_keep_alive == connections; }, 2s));

    // Every one of them idle, and the node answers at once. Until 0.43.0
    // sixteen of these would have been the whole worker pool.
    const auto started = Clock::now();
    const auto health = raw_http_get(server.bound_port(), "/api/v1/health");
    CHECK(Clock::now() - started < 500ms);
    CHECK(health.find("HTTP/1.1 200") != std::string::npos);
    CHECK(server.diagnostics().connections_open >= connections);

    for (const int fd : fds)
        ::close(fd);
    REQUIRE(wait_until([&] { return server.diagnostics().connections_open == 0; }, 5s));
    server.stop();
}

MACHA_TEST("http_server", test_a_client_that_closes_mid_body_releases_the_body_source_promptly) {
    auto config = loopback_config();
    auto destroyed = std::make_shared<std::atomic_bool>(false);
    HttpServer server(config, [&](const HttpRequest&) {
        HttpResponse response;
        response.content_type = "application/octet-stream";
        response.stream = std::make_shared<ResidentBody>(16 * 1024 * 1024, destroyed);
        return response;
    });
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() != 0; }, 1s));

    const int fd = connect_and_stall(server.bound_port(), "/big");
    // Read a little, so the response is genuinely in flight, then go away.
    std::array<char, 4096> buffer{};
    REQUIRE(::recv(fd, buffer.data(), buffer.size(), 0) > 0);
    REQUIRE(wait_until([&] { return server.diagnostics().connections_writing == 1; }, 2s));
    ::close(fd);

    // The reactor notices on its next pass and drops the pump with the
    // connection; the source is destroyed with it. Nothing waits for a
    // send timeout.
    CHECK(wait_until([&] { return destroyed->load(); }, 2s));
    CHECK(server.diagnostics().connections_open == 0);
    server.stop();
}

MACHA_TEST("http_server", test_a_deferred_request_is_resumed_when_woken_and_at_its_deadline) {
    auto config = loopback_config();
    std::mutex wakers_mutex;
    std::vector<std::shared_ptr<HttpWaker>> wakers;
    HttpServer server(config, [&](const HttpRequest& request) {
        if (request.path != "/wait")
            return http_error(404, "not_found", "not found");
        if (request.resumed) {
            const auto state = std::static_pointer_cast<int>(request.resumed_state);
            if (Clock::now() >= request.resume_deadline)
                return http_json(200, "{\"timed_out\":true}");
            return http_json(200, "{\"resumed\":true,\"state\":" + std::to_string(*state) + "}");
        }
        auto waker = std::make_shared<HttpWaker>();
        {
            std::lock_guard lock(wakers_mutex);
            wakers.push_back(waker);
        }
        HttpResponse deferred;
        deferred.defer = HttpDeferral{waker, Clock::now() + 400ms, std::make_shared<int>(42)};
        return deferred;
    });
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() != 0; }, 1s));

    // Woken: answered as soon as the producer fires, with its state back.
    std::string woken_response;
    const auto woken_started = Clock::now();
    std::jthread woken([&] { woken_response = raw_http_get(server.bound_port(), "/wait"); });
    REQUIRE(wait_until(
        [&] {
            std::lock_guard lock(wakers_mutex);
            return wakers.size() == 1;
        },
        2s));
    REQUIRE(wait_until([&] { return server.diagnostics().connections_deferred == 1; }, 2s));
    {
        std::lock_guard lock(wakers_mutex);
        wakers.front()->fire();
    }
    woken.join();
    CHECK(Clock::now() - woken_started < 400ms);
    CHECK(woken_response.find("\"resumed\":true") != std::string::npos);
    CHECK(woken_response.find("\"state\":42") != std::string::npos);

    // Never woken: re-run at the deadline, and the handler can tell.
    const auto timed_started = Clock::now();
    const auto timed_response = raw_http_get(server.bound_port(), "/wait");
    const auto timed_elapsed = Clock::now() - timed_started;
    CHECK(timed_response.find("\"timed_out\":true") != std::string::npos);
    CHECK(timed_elapsed >= 350ms);
    CHECK(timed_elapsed < 2s);

    CHECK(server.diagnostics().requests_deferred == 2);
    // A waker fired after its request has been answered is harmless.
    std::lock_guard lock(wakers_mutex);
    for (auto& waker : wakers)
        waker->fire();
    server.stop();
}

MACHA_TEST("http_server", test_reactor_stall_watchdog_counts_a_sleeping_pass) {
    auto config = loopback_config();
    config.reactor_stall_threshold = 20ms;
    HttpServer server(config, [](const HttpRequest&) { return http_json(200, "{\"ok\":true}"); });
    std::atomic_bool slept{};
    // The one thing the reactor must never do, done once on purpose.
    server.set_reactor_pass_hook([&] {
        if (!slept.exchange(true))
            std::this_thread::sleep_for(60ms);
    });
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() != 0; }, 1s));

    const auto response = raw_http_get(server.bound_port(), "/ok");
    CHECK(response.find("HTTP/1.1 200") != std::string::npos);
    REQUIRE(wait_until([&] { return server.diagnostics().reactor_stalls >= 1; }, 2s));
    CHECK(server.diagnostics().reactor_stalls == 1);
    CHECK(server.diagnostics().reactor_longest_pass_ms >= 50);
    server.stop();
}

MACHA_TEST("http_server", test_head_and_pipelined_requests_on_one_connection) {
    auto config = loopback_config();
    HttpServer server(config, [](const HttpRequest& request) {
        HttpResponse response;
        response.content_type = "application/octet-stream";
        response.stream = std::make_shared<ResidentBody>(1000);
        if (request.path == "/inline")
            return http_json(200, "{\"ok\":true}");
        return response;
    });
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() != 0; }, 1s));

    const int fd = connect_idle(server.bound_port());
    timeval timeout{2, 0};
    REQUIRE(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    // HEAD: the length of the body it is not sending, and the connection
    // stays usable because no body was ever owed. Read by hand, because the
    // shared reader would wait for the Content-Length bytes a HEAD answer
    // never carries.
    raw_http_send(fd, "HEAD /stream HTTP/1.1\r\nHost: localhost\r\n\r\n");
    std::string head;
    std::array<char, 4096> buffer{};
    while (head.find("\r\n\r\n") == std::string::npos) {
        const auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
        REQUIRE(n > 0);
        head.append(buffer.data(), static_cast<size_t>(n));
    }
    CHECK(head.starts_with("HTTP/1.1 200"));
    CHECK(head.find("Content-Length: 1000\r\n") != std::string::npos);
    CHECK(head.find("Accept-Ranges: bytes\r\n") != std::string::npos);
    CHECK(head.ends_with("\r\n\r\n")); // and nothing after the headers

    // Two requests in one write. The reactor parses the second out of the
    // bytes left over after the first, without waiting for more input.
    raw_http_send(fd, "GET /inline HTTP/1.1\r\nHost: localhost\r\n\r\n"
                      "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n");
    auto first = raw_http_read_response(fd);
    CHECK(first.status == 200);
    CHECK(first.body == "{\"ok\":true}");
    auto second = raw_http_read_response(fd);
    CHECK(second.status == 200);
    CHECK(second.body.size() == 1000);

    ::close(fd);
    server.stop();
}

} // namespace

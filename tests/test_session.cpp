// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_api.hpp"
#include "test_backend_support.hpp"

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

HttpRequest session_request(std::string method, std::optional<SessionIdentity> identity = {}) {
    HttpRequest request;
    request.method = std::move(method);
    request.path = "/api/v1/session";
    request.session = std::move(identity);
    return request;
}

MACHA_FAST_TEST("session", test_session_create_and_validate) {
    TestCluster cluster;
    NodeRuntime node(cluster.node_config("n1"), cluster.keys());
    SessionApi api(node);

    auto created = api.handle(session_request("POST"));
    REQUIRE(created.status == 201);
    auto body = Json::parse(std::string(created.body.begin(), created.body.end()));
    REQUIRE(body.find("id") != nullptr);
    REQUIRE(body.find("token") != nullptr);
    CHECK(body.find("token_type")->asString() == "Bearer");
    CHECK(!body.find("id")->asString().empty());
    CHECK(!body.find("token")->asString().empty());
    REQUIRE(body.find("roles")->isArray());
    REQUIRE(body.find("roles")->asArray().size() == 1);
    CHECK(body.find("roles")->asArray().front().asString() == "anonymous");
    CHECK(body.find("expires_unix_ms")->asUInt64() > body.find("created_unix_ms")->asUInt64());

    const auto token = body.find("token")->asString();
    const auto id = body.find("id")->asString();

    auto validated = node.sessions().validate(token);
    REQUIRE(validated.has_value());
    CHECK(validated->id == id);
    CHECK(session_has_role(*validated, "anonymous"));

    auto identity = session_identity(*validated);
    auto introspected = api.handle(session_request("GET", identity));
    REQUIRE(introspected.status == 200);
    auto introspected_body = Json::parse(std::string(introspected.body.begin(), introspected.body.end()));
    CHECK(introspected_body.find("id")->asString() == id);
    CHECK(introspected_body.find("token") == nullptr);
}

MACHA_FAST_TEST("session", test_session_rejects_non_anonymous_credentials) {
    TestCluster cluster;
    NodeRuntime node(cluster.node_config("n1"), cluster.keys());
    SessionApi api(node);

    auto request = session_request("POST");
    Json::Object credentials{{"username", "someone"}};
    Json::Object root{{"credentials", Json(std::move(credentials))}};
    auto text = Json(std::move(root)).dump();
    request.body.assign(text.begin(), text.end());

    auto response = api.handle(request);
    REQUIRE(response.status == 400);
    const std::string body(response.body.begin(), response.body.end());
    CHECK(body.find("unsupported_credentials") != std::string::npos);
}

MACHA_FAST_TEST("session", test_session_revoke_invalidates_immediately) {
    TestCluster cluster;
    NodeRuntime node(cluster.node_config("n1"), cluster.keys());
    SessionApi api(node);

    auto created = api.handle(session_request("POST"));
    REQUIRE(created.status == 201);
    auto body = Json::parse(std::string(created.body.begin(), created.body.end()));
    const auto token = body.find("token")->asString();

    auto validated = node.sessions().validate(token);
    REQUIRE(validated.has_value());
    auto identity = session_identity(*validated);

    auto revoked = api.handle(session_request("DELETE", identity));
    REQUIRE(revoked.status == 204);

    CHECK(!node.sessions().validate(token).has_value());
    auto introspected = api.handle(session_request("GET", identity));
    CHECK(introspected.status == 401);
}

MACHA_FAST_TEST("session", test_session_expiry) {
    TempDir dir;
    SessionManager manager(50ms, 16, dir.path() / "sessions.bin");
    auto minted_opt = manager.create({"anonymous"});
    REQUIRE(minted_opt.has_value());
    auto& minted = *minted_opt;
    REQUIRE(manager.validate(minted.bearer_token).has_value());

    std::this_thread::sleep_for(80ms);
    CHECK(!manager.validate(minted.bearer_token).has_value());

    manager.prune_expired(unix_ms());
    CHECK(manager.recent(1min).empty());
}

MACHA_FAST_TEST("session", test_session_create_enforces_max_sessions) {
    // Regression: a runaway client (observed live -- a broken proactive-
    // refresh timer that re-minted on every request) must hit a hard local
    // cap rather than growing this node's replica without bound. create()
    // used to skip the max_sessions check entirely (only apply(), the
    // gossip-received path, enforced it).
    SessionManager manager(1min, 2);
    REQUIRE(manager.create({"anonymous"}).has_value());
    REQUIRE(manager.create({"anonymous"}).has_value());
    CHECK(!manager.create({"anonymous"}).has_value());
    CHECK(manager.recent(1min).size() == 2);
}

std::optional<AuthSession> stored(const SessionManager& manager, const Hash256& token_hash) {
    // find()/validate() intentionally hide a revoked record (it must never
    // authenticate a request), so this test inspects the raw stored record
    // via recent() instead, which applies no such liveness filter.
    for (auto& session : manager.recent(1min))
        if (session.token_hash == token_hash) return session;
    return std::nullopt;
}

MACHA_FAST_TEST("session", test_session_lww_tie_prefers_revoked) {
    AuthSession base;
    base.id = "s1";
    base.token_hash = sha256(Bytes{1, 2, 3});
    base.roles = {"anonymous"};
    base.created_unix_ms = unix_ms();
    base.expires_unix_ms = unix_ms() + 60000;
    base.version = 5;

    auto revoked = base;
    revoked.revoked = true;

    {
        SessionManager manager(1min, 16);
        CHECK(manager.apply(base));
        // Same version, revoked side must win regardless of which arrived first.
        CHECK(manager.apply(revoked));
        auto found = stored(manager, base.token_hash);
        REQUIRE(found.has_value());
        CHECK(found->revoked);
    }
    {
        SessionManager manager(1min, 16);
        CHECK(manager.apply(revoked));
        // A non-revoked record at the same version must not un-revoke it.
        CHECK(!manager.apply(base));
        auto found = stored(manager, base.token_hash);
        REQUIRE(found.has_value());
        CHECK(found->revoked);
    }
}

MACHA_FAST_TEST("session", test_session_survives_persist_reload) {
    TempDir dir;
    const auto path = dir.path() / "sessions.bin";
    std::string token;
    {
        SessionManager manager(1min, 16, path);
        auto minted_opt = manager.create({"anonymous"});
        REQUIRE(minted_opt.has_value());
        auto& minted = *minted_opt;
        token = minted.bearer_token;
        manager.persist();
    }
    {
        SessionManager reloaded(1min, 16, path);
        auto validated = reloaded.validate(token);
        REQUIRE(validated.has_value());
        CHECK(session_has_role(*validated, "anonymous"));
    }
    // An already-expired session must not be reloaded as live.
    {
        SessionManager manager(50ms, 16, path);
        auto minted_opt = manager.create({"anonymous"});
        REQUIRE(minted_opt.has_value());
        auto& minted = *minted_opt;
        std::this_thread::sleep_for(80ms);
        manager.persist();
        SessionManager reloaded(50ms, 16, path);
        CHECK(!reloaded.validate(minted.bearer_token).has_value());
    }
}

MACHA_TEST("session", test_session_cluster_propagation) {
    TestCluster cluster;
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = cluster.node_config("n1", p1, {{"127.0.0.1", p2}});
    auto c2 = cluster.node_config("n2", p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.heartbeat = c2.heartbeat = 20ms;

    NodeRuntime n1(c1, cluster.keys());
    NodeRuntime n2(c2, cluster.keys());
    n1.start();
    n2.start();

    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    auto minted_opt = n1.sessions().create({"anonymous"});
    REQUIRE(minted_opt.has_value());
    auto& minted = *minted_opt;
    n1.propagate_session(minted.session);

    REQUIRE(wait_until([&] { return n2.sessions().validate(minted.bearer_token).has_value(); }, 2s));

    n2.stop();
    n1.stop();
}

MACHA_TEST("session", test_session_revoke_propagates_cluster_wide) {
    TestCluster cluster;
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = cluster.node_config("n1", p1, {{"127.0.0.1", p2}});
    auto c2 = cluster.node_config("n2", p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.heartbeat = c2.heartbeat = 20ms;

    NodeRuntime n1(c1, cluster.keys());
    NodeRuntime n2(c2, cluster.keys());
    n1.start();
    n2.start();

    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    auto minted_opt = n1.sessions().create({"anonymous"});
    REQUIRE(minted_opt.has_value());
    auto& minted = *minted_opt;
    n1.propagate_session(minted.session);
    REQUIRE(wait_until([&] { return n2.sessions().validate(minted.bearer_token).has_value(); }, 2s));

    auto revoked = n1.sessions().revoke(minted.session.token_hash);
    REQUIRE(revoked.has_value());
    n1.propagate_session(*revoked);

    REQUIRE(wait_until([&] { return !n2.sessions().validate(minted.bearer_token).has_value(); }, 2s));

    n2.stop();
    n1.stop();
}

MACHA_TEST("session", test_http_server_gate_uses_session_authenticator) {
    CatalogueApiConfig config;
    config.enabled = true;
    config.listen = "127.0.0.1";
    config.port = free_port();
    config.workers = 1;

    std::optional<std::string> observed_session_id;
    HttpServer server(
        config,
        [&](const HttpRequest& request) {
            if (request.session) observed_session_id = request.session->id;
            return http_json(200, "{\"ok\":true}");
        },
        [](const HttpRequest& request) { return request.path == "/exempt"; },
        [](std::string_view token) -> std::optional<SessionIdentity> {
            if (token != "good-token") return std::nullopt;
            return SessionIdentity{"the-session-id", Hash256{}, {"anonymous"}};
        });
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() == config.port; }, 2s));

    {
        const int fd = connect_idle(config.port);
        auto response = raw_http_exchange(fd, "GET /protected HTTP/1.1\r\nHost: localhost\r\n\r\n");
        CHECK(response.status == 401);
        ::close(fd);
    }
    {
        const int fd = connect_idle(config.port);
        auto response = raw_http_exchange(
            fd, "GET /protected HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer wrong\r\n\r\n");
        CHECK(response.status == 401);
        ::close(fd);
    }
    {
        const int fd = connect_idle(config.port);
        auto response = raw_http_exchange(
            fd,
            "GET /protected HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer good-token\r\n\r\n");
        CHECK(response.status == 200);
        ::close(fd);
    }
    REQUIRE(observed_session_id.has_value());
    CHECK(*observed_session_id == "the-session-id");
    {
        // bearer_exempt_ still bypasses entirely, with no Authorization header.
        const int fd = connect_idle(config.port);
        auto response = raw_http_exchange(fd, "GET /exempt HTTP/1.1\r\nHost: localhost\r\n\r\n");
        CHECK(response.status == 200);
        ::close(fd);
    }

    server.stop();
}

} // namespace

// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_api.hpp"

#include "log.hpp"

namespace macha {
namespace {

Json parse_body(const HttpRequest& request) {
    const std::string text(reinterpret_cast<const char*>(request.body.data()), request.body.size());
    if (text.empty())
        return Json(Json::Object{});
    auto root = Json::parse(text);
    if (!root.isObject())
        throw std::runtime_error("request body must be a JSON object");
    return root;
}

Json session_json(const AuthSession& session, std::string_view token = {}) {
    Json::Array roles;
    for (const auto& role : session.roles)
        roles.push_back(role);
    Json::Object out;
    out["id"] = session.id;
    out["roles"] = std::move(roles);
    out["created_unix_ms"] = static_cast<uint64_t>(session.created_unix_ms);
    out["expires_unix_ms"] = static_cast<uint64_t>(session.expires_unix_ms);
    if (!token.empty()) {
        out["token"] = std::string(token);
        out["token_type"] = "Bearer";
    }
    return Json(std::move(out));
}

} // namespace

std::optional<std::vector<std::string>> AnonymousCredentialValidator::validate(
    const Json& credentials) const {
    if (!credentials.isObject() || !credentials.asObject().empty())
        return std::nullopt; // any non-empty credentials must fail loudly -- not implemented yet
    return std::vector<std::string>{"anonymous"};
}

SessionApi::SessionApi(NodeRuntime& node, std::unique_ptr<CredentialValidator> validator)
    : sessions_(node.sessions()), node_(node), validator_(std::move(validator)) {}

bool SessionApi::capability_request(const HttpRequest& request) {
    return request.method == "POST" && request.path == "/api/v1/session";
}

HttpResponse SessionApi::handle(const HttpRequest& request) {
    try {
        if (request.path != "/api/v1/session")
            return http_error(404, "not_found", "session route not found");

        if (request.method == "POST") {
            const auto body = parse_body(request);
            std::vector<std::string> roles{"anonymous"};
            if (const auto* credentials = body.find("credentials")) {
                auto validated = validator_->validate(*credentials);
                if (!validated)
                    return http_error(400, "unsupported_credentials",
                                      "only empty (anonymous) credentials are supported");
                roles = std::move(*validated);
            }
            auto minted = sessions_.create(std::move(roles));
            if (!minted)
                return http_error(429, "too_many_sessions",
                                  "this node's session capacity is exhausted; retry shortly");
            node_.propagate_session(minted->session);
            return http_json(201, session_json(minted->session, minted->bearer_token).dump());
        }

        // Every other route under /api/v1/session operates on "my current
        // session" -- HttpServer has already authenticated the caller by the
        // time any non-exempt handler runs.
        if (!request.session)
            return http_error(401, "unauthorized", "a valid session bearer token is required");

        if (request.method == "GET") {
            auto session = sessions_.find(request.session->token_hash);
            if (!session)
                return http_error(401, "unauthorized", "session is no longer valid");
            return http_json(200, session_json(*session).dump());
        }

        if (request.method == "DELETE") {
            auto revoked = sessions_.revoke(request.session->token_hash);
            if (revoked)
                node_.propagate_session(*revoked);
            return {204, "application/json; charset=utf-8", {}, {}};
        }

        return http_error(405, "method_not_allowed", "unsupported method for /api/v1/session");
    } catch (const JsonError& error) {
        return http_error(400, "bad_json", error.what());
    } catch (const std::runtime_error& error) {
        return http_error(400, "bad_request", error.what());
    } catch (const std::exception& error) {
        Log::warn("session API failed: " + std::string(error.what()));
        return http_error(500, "internal_error", error.what());
    }
}

} // namespace macha

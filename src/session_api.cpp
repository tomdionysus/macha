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

Json session_json(const AuthSession& session, const UserStore& users,
                  std::string_view token = {}) {
    Json::Array roles;
    for (const auto& role : session.roles)
        roles.push_back(role);
    Json::Object out;
    out["id"] = session.id;
    out["roles"] = std::move(roles);
    out["created_unix_ms"] = static_cast<uint64_t>(session.created_unix_ms);
    out["expires_unix_ms"] = static_cast<uint64_t>(session.expires_unix_ms);
    if (!session.user_id.empty()) {
        out["user_id"] = session.user_id;
        // The name as well as the id: every client needs it on every load to
        // say who is signed in, and without it each one spends a round trip on
        // /users/me to learn something this response already knows.
        if (auto user = users.find(session.user_id))
            out["username"] = user->username;
    }
    // Stated, not inferred. A client validating a password inline must use the
    // server's rule rather than a copy of it, or the two drift and it starts
    // rejecting passwords the server would accept.
    Json::Object policy;
    policy["min_password_length"] = static_cast<uint64_t>(min_password_length);
    out["password_policy"] = std::move(policy);
    if (!token.empty()) {
        out["token"] = std::string(token);
        out["token_type"] = "Bearer";
    }
    return Json(std::move(out));
}

} // namespace

PasswordCredentialValidator::PasswordCredentialValidator(const UserStore& users,
                                                         SessionConfig config)
    : users_(users), config_(std::move(config)) {}

bool PasswordCredentialValidator::begin_check(const std::string& username) const {
    std::lock_guard lock(mutex_);
    if (auto found = failures_.find(username);
        found != failures_.end() && Clock::now() < found->second.until)
        return false;
    if (in_flight_ >= config_.max_concurrent_password_checks)
        return false;
    ++in_flight_;
    return true;
}

void PasswordCredentialValidator::end_check(const std::string& username, bool success) const {
    std::lock_guard lock(mutex_);
    if (in_flight_)
        --in_flight_;
    if (success) {
        failures_.erase(username);
        return;
    }
    auto& record = failures_[username];
    if (++record.count >= config_.failed_login_attempts) {
        record.count = 0;
        record.until = Clock::now() + config_.failed_login_lockout;
    }
}

CredentialResult PasswordCredentialValidator::validate(const Json& credentials) const {
    if (!credentials.isObject())
        return {CredentialOutcome::unsupported, {}};

    const auto& object = credentials.asObject();
    if (object.empty()) {
        if (!config_.allow_anonymous)
            return {CredentialOutcome::disabled, {}};
        // Anonymous is an ordinary account, so an anonymous session is an
        // ordinary bound session: it carries that account's current roles, and
        // it is retired by the same credential_generation check as anyone
        // else's the moment those roles change.
        auto user = users_.find_by_username(anonymous_username);
        if (!user || user->roles.empty())
            return {CredentialOutcome::disabled, {}};
        return {CredentialOutcome::ok, {user->roles, user->id, user->credential_generation}};
    }

    const auto* username = credentials.find("username");
    const auto* password = credentials.find("password");
    if (!username || !password || !username->isString() || !password->isString() ||
        object.size() != 2)
        return {CredentialOutcome::unsupported, {}};

    const auto name = normalize_username(username->asString());
    if (!begin_check(name))
        return {CredentialOutcome::rate_limited, {}};
    auto check = users_.verify(name, password->asString());
    end_check(name, check.ok);
    if (!check.ok)
        return {CredentialOutcome::rejected, {}};
    return {CredentialOutcome::ok,
            {std::move(check.roles), std::move(check.user_id), check.credential_generation}};
}

SessionApi::SessionApi(NodeRuntime& node, std::unique_ptr<CredentialValidator> validator)
    : sessions_(node.sessions()), node_(node),
      validator_(validator ? std::move(validator)
                           : std::make_unique<PasswordCredentialValidator>(
                                 node.users(), node.config().session)) {}

bool SessionApi::capability_request(const HttpRequest& request) {
    return request.method == "POST" && request.path == "/api/v1/session";
}

HttpResponse SessionApi::handle(const HttpRequest& request) {
    try {
        if (request.path != "/api/v1/session")
            return http_error(404, "not_found", "session route not found");

        if (request.method == "POST") {
            const auto body = parse_body(request);
            const auto* credentials = body.find("credentials");
            const auto result = credentials ? validator_->validate(*credentials)
                                            : validator_->validate(Json(Json::Object{}));
            switch (result.outcome) {
            case CredentialOutcome::unsupported:
                return http_error(400, "unsupported_credentials",
                                  "credentials must be empty (anonymous) or "
                                  "{username, password}");
            case CredentialOutcome::rejected:
                // Deliberately the same answer for an unknown user and a wrong
                // password; UserStore::verify spends the same time on both.
                return http_error(401, "invalid_credentials", "username or password is incorrect");
            case CredentialOutcome::disabled:
                return http_error(403, "anonymous_disabled",
                                  "this cluster requires a username and password");
            case CredentialOutcome::rate_limited: {
                auto response = http_error(429, "try_later",
                                           "too many password checks; retry shortly");
                response.headers["Retry-After"] = "1";
                return response;
            }
            case CredentialOutcome::ok:
                break;
            }

            auto minted = sessions_.create(result.credentials.roles, result.credentials.user_id,
                                           result.credentials.credential_generation);
            if (!minted)
                return http_error(429, "too_many_sessions",
                                  "this node's session capacity is exhausted; retry shortly");
            node_.propagate_session(minted->session);
            return http_json(201, session_json(minted->session, node_.users(), minted->bearer_token).dump());
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
            return http_json(200, session_json(*session, node_.users()).dump());
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

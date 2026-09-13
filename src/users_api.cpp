// SPDX-License-Identifier: GPL-3.0-or-later
#include "users_api.hpp"

#include "log.hpp"

#include <algorithm>

namespace macha {
namespace {
constexpr std::string_view users_root = "/api/v1/users";
constexpr std::string_view users_prefix = "/api/v1/users/";

Json parse_body(const HttpRequest& request) {
    const std::string text(reinterpret_cast<const char*>(request.body.data()), request.body.size());
    if (text.empty())
        return Json(Json::Object{});
    auto root = Json::parse(text);
    if (!root.isObject())
        throw std::runtime_error("request body must be a JSON object");
    return root;
}

// A password hash, its salt and its KDF parameters never leave the node: the
// API can create and replace a credential but has no route that reads one
// back, so a compromised admin token cannot exfiltrate the table for offline
// cracking.
Json user_json(const UserRecord& user, const UserMutability& mutability) {
    Json::Array roles;
    for (const auto& role : user.roles)
        roles.push_back(role);
    Json::Object out;
    out["id"] = user.id;
    out["username"] = user.username;
    out["roles"] = std::move(roles);
    out["created_unix_ms"] = static_cast<uint64_t>(user.created_unix_ms);
    out["updated_unix_ms"] = static_cast<uint64_t>(user.updated_unix_ms);
    out["credential_generation"] = static_cast<uint64_t>(user.credential_generation);
    // The record's LWW counter, for If-Match style optimistic concurrency.
    out["version"] = static_cast<uint64_t>(user.version);

    // Stated per field, so a client renders exactly what it is told rather than
    // reimplementing the rules and drifting from them.
    Json::Object may;
    may["rename"] = mutability.rename;
    may["delete"] = mutability.remove;
    may["set_password"] = mutability.set_password;
    may["set_roles"] = mutability.set_roles;
    Json::Array required;
    for (const auto& role : mutability.required_roles)
        required.push_back(role);
    may["required_roles"] = std::move(required);
    if (mutability.last_user_manager)
        may["required_roles_reason"] = std::string("last_user_manager");
    out["mutable"] = std::move(may);
    return Json(std::move(out));
}

std::optional<std::vector<std::string>> read_roles(const Json& body) {
    const auto* roles = body.find("roles");
    if (!roles)
        return std::nullopt;
    if (!roles->isArray())
        throw std::runtime_error("roles must be a list");
    std::vector<std::string> out;
    for (const auto& role : roles->asArray()) {
        if (!role.isString())
            throw std::runtime_error("roles must be a list of strings");
        if (!known_role(role.asString()))
            throw std::runtime_error("unknown role '" + role.asString() + "'");
        out.push_back(role.asString());
    }
    return out;
}

std::string read_password(const Json& body) {
    const auto* password = body.find("password");
    if (!password)
        return {};
    if (!password->isString() || password->asString().empty())
        throw std::runtime_error("password must be a non-empty string");
    return password->asString();
}
} // namespace

UserMutability UsersApi::mutability(const UserRecord& user) const {
    UserMutability out;
    const bool reserved = reserved_username(user.username);
    // root and anonymous are permanent fixtures: root is the way back in when
    // every other account is locked out, and anonymous is what an
    // unauthenticated visitor is.
    out.rename = !reserved;
    out.remove = !reserved;
    // Roles stay editable on both: anonymous's roles are the only control over
    // what an unauthenticated television can reach. Passwords are ordinary on
    // root and refused on anonymous, which has none by construction -- being
    // able to log in as it would be a way past `allow_anonymous: false`, and
    // before 0.38.4 any visitor holding an anonymous session could set that
    // password through /api/v1/users/me and make one.
    out.set_password = user.username != anonymous_username;
    out.set_roles = true;

    if (user_has_role(user, role_manage_users) && node_.users().sole_user_manager(user.id)) {
        // Not "these roles are frozen": this account's roles stay editable, and
        // only manage_users is pinned to it. Dropping it here would leave a
        // cluster nobody can administer, and that edit would replicate
        // perfectly -- there is no undo, because undoing it is the thing that
        // just became impossible.
        out.required_roles.emplace_back(role_manage_users);
        out.remove = false;
        out.last_user_manager = true;
    }
    return out;
}

bool UsersApi::routes(std::string_view path) {
    return path == users_root || path.starts_with(users_prefix);
}

HttpResponse UsersApi::create(const HttpRequest& request) {
    const auto body = parse_body(request);
    const auto* username = body.find("username");
    if (!username || !username->isString() || username->asString().empty())
        return http_error(400, "bad_request", "username is required");
    const auto password = read_password(body);
    if (password.empty())
        return http_error(400, "password_required", "a password is required");
    if (password.size() < min_password_length)
        return http_error(400, "password_rejected",
                          "password must be at least " + std::to_string(min_password_length) +
                              " characters");
    if (reserved_username(username->asString()))
        return http_error(409, "reserved_username",
                          "'root' and 'anonymous' are reserved names");
    auto roles =
        read_roles(body).value_or(std::vector<std::string>{std::string(role_media_viewer)});

    auto created = node_.users().create(username->asString(), password, roles, node_.node_id());
    if (!created) {
        if (node_.users().find_by_username(username->asString()))
            return http_error(409, "username_taken", "that username already exists");
        return http_error(507, "too_many_users", "the cluster user table is full");
    }
    node_.propagate_users();
    auto response = http_json(201, user_json(*created, mutability(*created)).dump());
    response.headers["Location"] = std::string(users_prefix) + created->id;
    return response;
}

HttpResponse UsersApi::update(const HttpRequest& request, const std::string& user_id, bool self) {
    const auto body = parse_body(request);
    const auto password = read_password(body);
    if (!password.empty() && password.size() < min_password_length)
        return http_error(400, "password_rejected",
                          "password must be at least " + std::to_string(min_password_length) +
                              " characters");
    if (!password.empty()) {
        auto target = node_.users().find(user_id);
        if (target && target->username == anonymous_username)
            return http_error(409, "no_password",
                              "the 'anonymous' account has no password and cannot be given "
                              "one; anonymous access is controlled by its roles and by "
                              "session.allow_anonymous");
    }
    auto roles = read_roles(body);
    if (self && roles)
        // Otherwise changing your own password would be a privilege-escalation
        // route for anyone who can reach /me -- which is everyone.
        return http_error(403, "forbidden",
                          "roles can only be changed by an account with manage_users");
    if (password.empty() && !roles)
        return http_error(400, "bad_request", "nothing to change");

    if (roles) {
        auto target = node_.users().find(user_id);
        if (target && user_has_role(*target, role_manage_users) &&
            std::find(roles->begin(), roles->end(), role_manage_users) == roles->end()) {
            size_t managers = 0;
            for (const auto& other : node_.users().list())
                if (user_has_role(other, role_manage_users))
                    ++managers;
            if (managers <= 1)
                return http_error(409, "last_user_manager",
                                  "this is the only account that can manage users; "
                                  "grant manage_users to another account first");
        }
    }

    auto updated = node_.users().update(user_id, password, roles, node_.node_id());
    if (!updated)
        return http_error(404, "not_found", "no such user");
    node_.propagate_users();

    auto payload = user_json(*updated, mutability(*updated));
    // Changing a password bumps credential_generation, which invalidates every
    // session minted against the old one -- including the caller's own. Hand
    // back a fresh session so a person is not logged out by their own change.
    if (!password.empty() && self) {
        if (auto minted = node_.sessions().create(updated->roles, updated->id,
                                                  updated->credential_generation)) {
            node_.propagate_session(minted->session);
            payload.asObject()["token"] = minted->bearer_token;
            payload.asObject()["token_type"] = "Bearer";
            payload.asObject()["session_id"] = minted->session.id;
        }
    }
    return http_json(200, payload.dump());
}

HttpResponse UsersApi::remove(const HttpRequest& request, const std::string& user_id) {
    if (request.session && request.session->user_id == user_id)
        return http_error(409, "cannot_delete_self", "you cannot delete your own account");
    auto target = node_.users().find(user_id);
    // root is the account that can always administer this cluster; it is the
    // one way back in when every other account has been locked out, misroled
    // or forgotten. Change its password, do not remove it.
    if (target && reserved_username(target->username))
        return http_error(409, "reserved_user",
                          "the '" + target->username +
                              "' account cannot be removed; change its password instead");
    // Removing the last account that can manage accounts would replicate
    // perfectly and leave nobody able to undo it.
    if (target && user_has_role(*target, role_manage_users)) {
        size_t remaining = 0;
        for (const auto& user : node_.users().list())
            if (user_has_role(user, role_manage_users))
                ++remaining;
        if (remaining <= 1)
            return http_error(409, "last_user_manager",
                              "this is the only account that can manage users; "
                              "grant manage_users to another account first");
    }
    auto removed = node_.users().remove(user_id, node_.node_id());
    if (!removed)
        return http_error(404, "not_found", "no such user");
    node_.propagate_users();
    return {204, "application/json; charset=utf-8", {}, {}};
}

HttpResponse UsersApi::handle(const HttpRequest& request) {
    try {
        if (!request.session)
            return http_error(401, "unauthorized", "a valid session bearer token is required");

        if (request.path == users_root) {
            if (request.method == "GET") {
                Json::Array out;
                for (const auto& user : node_.users().list())
                    out.push_back(user_json(user, mutability(user)));
                Json::Object body;
                body["users"] = std::move(out);
                return http_json(200, Json(std::move(body)).dump());
            }
            if (request.method == "POST")
                return create(request);
            return http_error(405, "method_not_allowed", "unsupported method for /api/v1/users");
        }

        const auto tail = request.path.substr(users_prefix.size());
        if (tail.empty() || tail.find('/') != std::string::npos)
            return http_error(404, "not_found", "user route not found");

        // "me" is the caller's own account, and is the only user route a
        // non-admin can reach (the role gate lets viewers through to it).
        const bool self = tail == "me";
        if (self && request.session->user_id.empty())
            return http_error(404, "no_account",
                              "this is an anonymous session; it has no user account");
        const auto user_id = self ? request.session->user_id : tail;

        if (request.method == "GET") {
            auto user = node_.users().find(user_id);
            if (!user)
                return http_error(404, "not_found", "no such user");
            return http_json(200, user_json(*user, mutability(*user)).dump());
        }
        if (request.method == "PATCH")
            return update(request, user_id, self);
        if (request.method == "DELETE") {
            if (self)
                return http_error(405, "method_not_allowed",
                                  "delete your account by id, as an admin");
            return remove(request, user_id);
        }
        return http_error(405, "method_not_allowed", "unsupported method for this user");
    } catch (const JsonError& error) {
        return http_error(400, "bad_json", error.what());
    } catch (const std::runtime_error& error) {
        return http_error(400, "bad_request", error.what());
    } catch (const std::exception& error) {
        Log::warn("users API failed: " + std::string(error.what()));
        return http_error(500, "internal_error", error.what());
    }
}

} // namespace macha

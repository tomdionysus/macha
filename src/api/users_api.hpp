// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "auth/accounts.hpp"
#include "cluster/cluster.hpp"
#include "http/http.hpp"
#include "json.hpp"

namespace macha {

// Admin surface for the cluster user table, plus /api/v1/users/me for the
// caller's own account. Every mutation is local-then-notify: apply to this
// replica, persist, queue to reachable peers, return. Nothing waits on a peer.
class UsersApi {
    NodeRuntime& node_;
    Accounts& accounts_;

    UserMutability mutability(const UserRecord&) const;
    HttpResponse create(const HttpRequest&);
    HttpResponse update(const HttpRequest&, const std::string& user_id, bool self);
    HttpResponse remove(const HttpRequest&, const std::string& user_id);

  public:
    UsersApi(NodeRuntime& node, Accounts& accounts) : node_(node), accounts_(accounts) {}
    HttpResponse handle(const HttpRequest&);
    static bool routes(std::string_view path);

};

} // namespace macha

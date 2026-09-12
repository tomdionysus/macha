// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster.hpp"
#include "http.hpp"
#include "json.hpp"

namespace macha {

// Admin surface for the cluster user table, plus the one route a person needs
// for their own account (/api/v1/users/me). Every mutation is local-then-
// notify: apply to this node's replica, persist, queue the table to whatever
// peers are reachable, return. Nothing here waits on a peer, so it works on a
// node that is temporarily alone.
class UsersApi {
    NodeRuntime& node_;

    UserMutability mutability(const UserRecord&) const;
    HttpResponse create(const HttpRequest&);
    HttpResponse update(const HttpRequest&, const std::string& user_id, bool self);
    HttpResponse remove(const HttpRequest&, const std::string& user_id);

  public:
    explicit UsersApi(NodeRuntime& node) : node_(node) {}
    HttpResponse handle(const HttpRequest&);
    static bool routes(std::string_view path);

};

} // namespace macha

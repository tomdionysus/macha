// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>

#include "auth/password_work.hpp"
#include "auth/session.hpp"
#include "auth/users.hpp"
#include "cluster/message_routes.hpp"
#include "cluster/node_identity.hpp"
#include "config.hpp"
#include "contract/thread_safety.hpp"

#include <dannan/context.h>

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace macha {

class NodeRuntime;

// This cluster's accounts as this node holds them: the user table and
// sessions, kept in step with peers. Mutations are pushed at once; a gossip
// thread is the backstop, at the telemetry cadence and whenever a peer joins
// or restarts (so it learns the table promptly). Built by the root after the
// node, whose transport carries the gossip; binds the user_sync and
// session_sync routes on construction and unbinds them on destruction.
class Accounts {
  public:
    Accounts(const Config&, const NodeIdentity&, NodeRuntime&, MessageRoutes&);
    ~Accounts();
    Accounts(const Accounts&) = delete;
    Accounts& operator=(const Accounts&) = delete;

    // Creates the first accounts on a node founding a cluster, warns when a
    // joining node holds none, and starts the gossip. Call after the node
    // has started.
    void start();
    // Stops the gossip; call before stopping the node. Idempotent.
    void stop();

    UserStore& users() noexcept {
        return users_;
    }
    const UserStore& users() const noexcept {
        return users_;
    }
    SessionManager& sessions() noexcept {
        return sessions_;
    }
    const SessionManager& sessions() const noexcept {
        return sessions_;
    }
    PasswordWork& password_work() noexcept {
        return password_work_;
    }

    bool apply_session(const AuthSession&);
    // Applies locally, then notifies peers without waiting on them. A peer
    // the notification could not be queued for is told by the gossip loop,
    // woken for it.
    void propagate_session(const AuthSession&);
    // Sends the whole table, tombstones included, to every peer, likewise.
    void propagate_users();

  private:
    void bind_routes();
    void gossip_loop(const std::stop_token&);
    void gossip_sessions();
    void gossip_users_if_changed();
    void request_gossip();
    // On the roster watch's strand: a peer new, or come back as a new
    // incarnation, is gossiped to.
    void on_roster_change(const dannan::RosterChange&);
    std::set<NodeId> active_peers() const;

    const Config& cfg_;
    const NodeIdentity& identity_;
    NodeRuntime& node_;
    MessageRoutes& routes_;
    // The roster watch that wakes the gossip for a peer joining or restarting,
    // and each peer's incarnation as it last told it (its strand's only).
    std::optional<uint64_t> roster_watch_;
    std::map<NodeId, dannan::identity::NodeId> peer_incarnations_;
    // Guards nothing: the gossip thread's wait lock.
    Mutex wait_mutex_;
    std::condition_variable_any wait_cv_;
    SessionManager sessions_;
    UserStore users_;
    PasswordWork password_work_;
    std::chrono::milliseconds gossip_ttl_;
    // What the last gossip broadcast said, and which peers have been told it.
    // A set of ids rather than a count, so membership churn does not
    // re-broadcast.
    Hash256 gossiped_user_table_{};
    std::set<NodeId> gossiped_user_peers_;
    Clock::time_point gossip_users_retry_after_{};
    Clock::time_point gossiped_users_at_{};
    Hash256 gossiped_sessions_{};
    std::set<NodeId> gossiped_session_peers_;
    Clock::time_point gossip_sessions_retry_after_{};
    Clock::time_point gossiped_sessions_at_{};
    // Raised by a push that missed a peer; the gossip loop wakes for it.
    std::atomic_uint64_t gossip_demand_{};
    std::vector<MessageType> bound_;
    std::jthread gossip_;
};

} // namespace macha

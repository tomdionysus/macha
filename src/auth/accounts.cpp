// SPDX-License-Identifier: GPL-3.0-or-later
#include "auth/accounts.hpp"

#include "cluster/cluster.hpp"
#include "crypto.hpp"
#include "diagnostics.hpp"
#include "log.hpp"
#include "supervised.hpp"

#include <algorithm>

namespace macha {
namespace {

// Retry delay for a gossip broadcast that missed a peer: long enough not to
// become a load source, short enough for a rejoining node to converge promptly.
constexpr auto gossip_retry_floor = std::chrono::seconds(1);
// Re-announce this often even when nothing changed and every peer is
// believed told: broadcast_notify() counts frames queued, not applied,
// so a peer whose inbound route is not yet usable (as while it restarts) can
// be marked told without receiving anything. Announcing on change is the
// optimisation; this is the guarantee.
constexpr auto gossip_reannounce_interval = std::chrono::seconds(30);

} // namespace

Accounts::Accounts(const Config& cfg, const NodeIdentity& identity, NodeRuntime& node,
                   NodeEvents& events, MessageRoutes& routes)
    : cfg_(cfg), identity_(identity), node_(node), events_(events), routes_(routes),
      sessions_(cfg.session.anonymous_ttl, cfg.session.max_sessions,
                cfg.state_path / "sessions" / "sessions.bin"),
      users_(cfg.session.max_users, cfg.state_path / "users" / "users.bin",
             hkdf_sha256(identity.keys.master, {},
                         std::span<const uint8_t>(
                             reinterpret_cast<const uint8_t*>("macha/users/v1"), 14))),
      password_work_(cfg.session.max_concurrent_password_checks),
      gossip_ttl_(std::max(cfg.dead_after * 2, std::chrono::milliseconds(60000))) {
    bind_routes();
}

Accounts::~Accounts() {
    stop();
    for (const auto type : bound_)
        routes_.unbind(type);
}

void Accounts::start() {
    // A node with no bootstrap peers founds the cluster (the same test
    // MetadataReplica uses for genesis authority): the only moment an account
    // may be created without one to authorise it. A joining node with
    // bootstrap peers and an empty user table cannot authenticate anything
    // (403 everywhere), so it says so loudly.
    if (!cfg_.bootstrap.empty() && users_.all().empty()) {
        Log::warn("accounts: this node holds no user accounts, so nothing can sign in and "
                  "every API route will refuse with 403");
        Log::warn("accounts: if this cluster has just been upgraded, stop one node and run: "
                  "macha-users " + cfg_.state_path.string() + " <cluster.key> init");
        Log::warn("accounts: if it has not, this node has simply not received the user table "
                  "yet and will converge shortly");
    }
    if (cfg_.bootstrap.empty()) {
        if (auto initial =
                create_initial_accounts(users_, identity_.keys, cfg_.state_path, identity_.id)) {
            // The password is in the 0600 file, never in a log line.
            Log::warn("accounts: created the '" + initial->root.username + "' and '" +
                      initial->anonymous.username + "' accounts for this new cluster");
            Log::warn("accounts: the generated " + initial->root.username + " password is in " +
                      initial->path.string() + " -- sign in, change it, delete that file");
            propagate_users();
        }
    }
    if (gossip_.joinable())
        return;
    gossip_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("accounts-gossip", stop, [this, stop] { gossip_loop(stop); });
    });
}

void Accounts::stop() {
    if (!gossip_.joinable())
        return;
    gossip_.request_stop();
    {
        Lock lock(events_.wait_mutex);
    }
    events_.wait_cv.notify_all();
    gossip_.join();
}

void Accounts::bind_routes() {
    const auto route = [this](MessageType type, MessageRoutes::Handler handler) {
        routes_.bind(type, std::move(handler));
        bound_.push_back(type);
    };
    route(MessageType::user_sync, [this](const NodeInfo&, FrameType, const RpcMessage& request) {
        if (!request.payload.empty())
            (void)users_.apply_all(decode_users(request.payload));
        return RpcMessage{MessageType::user_sync_reply, encode_users(users_.all())};
    });
    route(MessageType::session_sync,
          [this](const NodeInfo&, FrameType, const RpcMessage& request) {
              if (!request.payload.empty())
                  for (const auto& session : decode_sessions(request.payload))
                      apply_session(session);
              return RpcMessage{MessageType::session_sync_reply,
                                encode_sessions(sessions_.recent(gossip_ttl_, 64))};
          });
}

bool Accounts::apply_session(const AuthSession& session) {
    return sessions_.apply(session);
}

void Accounts::propagate_session(const AuthSession& session) {
    (void)apply_session(session);
    // Notify, never call: a synchronous call would stall every login on an
    // unreachable-but-not-dead peer, exactly when logging in matters. The
    // local merge is done and the gossip tick is the backstop.
    try {
        const auto reached = node_.broadcast_notify(
            {MessageType::session_sync, encode_sessions({session})}, FrameType::control);
        if (reached < active_peers().size())
            request_gossip();
    } catch (const std::exception& error) {
        Log::debug("session propagation skipped: " + std::string(error.what()));
        request_gossip();
    }
}

void Accounts::request_gossip() {
    gossip_demand_.fetch_add(1, std::memory_order_acq_rel);
    {
        Lock lock(events_.wait_mutex);
    }
    events_.wait_cv.notify_all();
}

bool Accounts::apply_user(const UserRecord& user) {
    return users_.apply(user);
}

void Accounts::propagate_users() {
    // Always the full table: a peer offline longer than the gossip TTL must
    // still converge, and at tens of records it is smaller than the telemetry set.
    try {
        auto values = users_.all();
        if (values.empty())
            return;
        const auto reached = node_.broadcast_notify(
            {MessageType::user_sync, encode_users(values)}, FrameType::control);
        if (reached < active_peers().size())
            request_gossip();
    } catch (const std::exception& error) {
        Log::debug("user propagation skipped: " + std::string(error.what()));
        request_gossip();
    }
}

std::set<NodeId> Accounts::active_peers() const {
    std::set<NodeId> peers;
    for (const auto& peer : node_.membership().active())
        if (peer.id != identity_.id)
            peers.insert(peer.id);
    return peers;
}

void Accounts::gossip_loop(std::stop_token stop) {
    // `network.telemetry_interval_ms`, floored so a mis-set value cannot spin.
    const auto interval = std::max(cfg_.telemetry_interval, std::chrono::milliseconds(250));
    while (!stop.stop_requested()) {
        const auto topology = events_.count(NodeEvent::topology);
        const auto demand = gossip_demand_.load(std::memory_order_acquire);
        gossip_sessions();
        // The whole user table, tombstones included, so a node that missed a
        // deletion learns the tombstone rather than resurrecting the account.
        gossip_users_if_changed();
        // A broadcast that missed a peer is retried at its floor, not a tick
        // later.
        auto wait = std::chrono::duration_cast<Clock::duration>(interval);
        const auto now = Clock::now();
        for (const auto retry : {gossip_sessions_retry_after_, gossip_users_retry_after_})
            if (retry > now)
                wait = std::min(wait, retry - now);
        Lock lock(events_.wait_mutex);
        events_.wait_cv.wait_for(lock.native(), stop, wait, [&] {
            return events_.count(NodeEvent::topology) != topology ||
                   gossip_demand_.load(std::memory_order_acquire) != demand;
        });
    }
}

void Accounts::gossip_sessions() {
    // Sessions are pushed on mutation (propagate_session); this is the
    // backstop for a peer unreachable at the time.
    try {
        sessions_.prune_expired(unix_ms());
        auto values = sessions_.recent(gossip_ttl_, 64);
        if (values.empty())
            return;
        // As for the user table: each notification costs every peer a
        // bounded queue admission, so an unchanged set is not resent.
        auto payload = encode_sessions(values);
        const auto digest = sha256(payload);
        auto peers = active_peers();
        const auto now = Clock::now();
        const bool due = digest != gossiped_sessions_ || peers != gossiped_session_peers_ ||
                         now - gossiped_sessions_at_ >= gossip_reannounce_interval;
        if (!due || now < gossip_sessions_retry_after_)
            return;
        const auto reached = node_.broadcast_notify(
            {MessageType::session_sync, std::move(payload)}, FrameType::control);
        // Recorded only once it reached everyone: a best-effort notify queues
        // nothing for a busy or not-yet-usable peer (as when a peer rejoins),
        // and recording anyway would retire the retry before it ran.
        if (reached >= peers.size()) {
            gossiped_sessions_ = digest;
            gossiped_session_peers_ = std::move(peers);
            gossiped_sessions_at_ = now;
        } else {
            // Retry on a floor, not every tick: an unreached peer is usually
            // busy, and gossip must not add load to it.
            gossip_sessions_retry_after_ = Clock::now() + gossip_retry_floor;
        }
    } catch (const std::exception& error) {
        Log::debug("session gossip skipped: " + std::string(error.what()));
    }
}

void Accounts::gossip_users_if_changed() {
    // Every notification costs each peer an admission slot in its bounded
    // request queue, most on a busy node, so an unchanged table sends
    // nothing. A broadcast goes out when the table changed here or a peer
    // appeared that may have missed it (how a node that was down converges:
    // the whole table, tombstones included, goes out once).
    try {
        const auto table = users_.table_hash();
        auto peers = active_peers();
        const auto now = Clock::now();
        if (table == gossiped_user_table_ && peers == gossiped_user_peers_ &&
            now - gossiped_users_at_ < gossip_reannounce_interval)
            return;
        if (now < gossip_users_retry_after_)
            return;
        auto values = users_.all();
        if (values.empty())
            return;
        const auto reached = node_.broadcast_notify(
            {MessageType::user_sync, encode_users(values)}, FrameType::control);
        // Commit only when it reached everyone, so a not-yet-usable peer is
        // retried next tick rather than marked told.
        if (reached >= peers.size()) {
            gossiped_user_table_ = table;
            gossiped_user_peers_ = std::move(peers);
            gossiped_users_at_ = now;
        } else {
            gossip_users_retry_after_ = Clock::now() + gossip_retry_floor;
        }
    } catch (const std::exception& error) {
        Log::debug("user gossip skipped: " + std::string(error.what()));
    }
}

} // namespace macha

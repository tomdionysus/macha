// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster_jobs.hpp"

#include "cluster.hpp"
#include "json.hpp"
#include "log.hpp"
#include "supervised.hpp"
#include "codec.hpp"

#include <algorithm>

namespace macha {
namespace {

Json parse_payload(const RpcReply& reply) {
    const std::string text(reinterpret_cast<const char*>(reply.message.payload.data()),
                           reply.message.payload.size());
    return Json::parse(text);
}

std::string error_text(const RpcReply& reply) {
    try {
        Reader reader(reply.message.payload);
        return reader.string();
    } catch (const std::exception&) {
        return {};
    }
}

Bytes json_bytes(Json value) {
    const auto text = value.dump();
    return Bytes(text.begin(), text.end());
}

std::optional<StagingStatus> parse_staging(const Json* value) {
    if (!value || !value->isObject()) return std::nullopt;
    StagingStatus out;
    if (const auto* v = value->find("limit_bytes")) out.limit = v->asUInt64();
    if (const auto* v = value->find("disk_bytes")) out.disk_bytes = v->asUInt64();
    if (const auto* v = value->find("reserved_bytes")) out.reserved_bytes = v->asUInt64();
    if (const auto* v = value->find("accounted_bytes")) out.accounted_bytes = v->asUInt64();
    return out;
}

// A peer that answers the torrent query at all runs torrents; one without the
// plugin answers with this error (NodeRuntime::handle_request).
bool says_no_torrents(const std::string& error) {
    return error.find("torrents not available") != std::string::npos;
}

} // namespace

ClusterJobView::ClusterJobView(NodeRuntime& node, IngestManager& ingest, SubsystemRegistry& registry,
                               std::chrono::milliseconds refresh_interval)
    : node_(node), ingest_(ingest), registry_(registry), refresh_interval_(refresh_interval) {
    reconfigure(node_.config().torrent);
}

void ClusterJobView::reconfigure(const TorrentConfig& config) {
    default_remove_after_ms_.store(
        config.remove_on_complete_after ? config.remove_on_complete_after->count() : -1,
        std::memory_order_relaxed);
}

ClusterJobView::~ClusterJobView() { stop(); }

void ClusterJobView::start() {
    if (worker_.joinable()) return;
    worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("cluster-job-view", stop, [this, stop] { loop(stop); });
    });
}

void ClusterJobView::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    wake_.notify_all();
    worker_.join();
}

void ClusterJobView::loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        refresh_now();
        std::mutex wait_mutex;
        std::unique_lock lock(wait_mutex);
        wake_.wait_for(lock, stop, refresh_interval_, [] { return false; });
    }
}

void ClusterJobView::refresh_now() {
    std::lock_guard polling(poll_mutex_);
    const auto self = node_.node_id();
    std::vector<NodeInfo> peers;
    for (const auto& peer : node_.membership().active())
        if (peer.id != self) peers.push_back(peer);
    {
        // A node that left membership keeps its last jobs, marked unreachable:
        // shown stale is better than silently gone.
        std::lock_guard lock(mutex_);
        for (auto& [id, peer] : peers_)
            if (std::none_of(peers.begin(), peers.end(), [&](const NodeInfo& p) { return p.id == id; }))
                peer.reachable = false;
    }
    for (const auto& peer : peers) poll(peer);
}

void ClusterJobView::poll(const NodeInfo& info) {
    Peer fresh;
    fresh.host = info.host;
    bool reached = false;
    try {
        auto reply = node_.call(info, MessageType::get_torrent_jobs, {}, FrameType::control);
        reached = true;
        if (reply.message.type == MessageType::torrent_jobs_reply) {
            auto parsed = parse_payload(reply);
            fresh.torrent_known = fresh.torrent_capable = true;
            if (const auto* jobs = parsed.find("jobs"))
                for (const auto& value : jobs->asArray()) fresh.torrents.push_back(parse_torrent_job_wire(value));
            if (const auto* node = parsed.find("node"); node && node->isObject()) {
                if (const auto* v = node->find("accepting"); v && v->isBool()) fresh.offer.accepting = v->asBool();
                if (const auto* v = node->find("not_accepting_reason"); v && v->isString())
                    fresh.offer.not_accepting_reason = v->asString();
                if (const auto* v = node->find("max_active")) fresh.offer.max_active = v->asUInt64();
                if (const auto* v = node->find("active_jobs")) fresh.offer.active_jobs = v->asUInt64();
                fresh.staging = parse_staging(node->find("staging"));
            } else {
                // A pre-0.64.0 peer: it runs torrents but says nothing of its
                // room. Offer it, and let the add answer for itself.
                fresh.offer.accepting = true;
            }
        } else if (reply.message.type == MessageType::error && says_no_torrents(error_text(reply))) {
            fresh.torrent_known = true;
        }
    } catch (const std::exception& error) {
        Log::debug("cluster job view: torrent query to " + info.host + ": " + error.what());
    }
    try {
        auto reply = node_.call(info, MessageType::get_ingest_jobs, {}, FrameType::control);
        reached = true;
        if (reply.message.type == MessageType::ingest_jobs_reply) {
            auto parsed = parse_payload(reply);
            fresh.ingest_known = true;
            if (const auto* jobs = parsed.find("jobs"))
                for (const auto& value : jobs->asArray()) fresh.ingests.push_back(parse_ingest_job_wire(value));
        }
    } catch (const std::exception& error) {
        Log::debug("cluster job view: ingest query to " + info.host + ": " + error.what());
    }

    std::lock_guard lock(mutex_);
    auto& peer = peers_[info.id];
    peer.host = info.host;
    if (!reached) {
        peer.reachable = false; // keep what it last said
        return;
    }
    fresh.reachable = true;
    fresh.as_of_unix_ms = unix_ms();
    // A half-answered poll keeps the unanswered half from last time.
    if (!fresh.torrent_known && peer.torrent_known) {
        fresh.torrent_known = true;
        fresh.torrent_capable = peer.torrent_capable;
        fresh.torrents = std::move(peer.torrents);
        fresh.offer = peer.offer;
        fresh.staging = peer.staging;
    }
    if (!fresh.ingest_known && peer.ingest_known) {
        fresh.ingest_known = true;
        fresh.ingests = std::move(peer.ingests);
    }
    peer = std::move(fresh);
}

NodeId ClusterJobView::local_node_id() const { return node_.node_id(); }

std::optional<NodeInfo> ClusterJobView::active_peer(const NodeId& id) const {
    for (const auto& peer : node_.membership().active())
        if (peer.id == id) return peer;
    return std::nullopt;
}

// ---- torrents ---------------------------------------------------------------

ClusterJobView::TorrentListing ClusterJobView::torrent_jobs() const {
    TorrentListing out;
    const auto self = node_.node_id();
    if (auto local = registry_.torrent()) {
        for (auto& job : local->jobs()) out.jobs.push_back({self, std::move(job)});
        out.sources.push_back({self, true, true, unix_ms()});
    }
    std::lock_guard lock(mutex_);
    for (const auto& [id, peer] : peers_) {
        if (!peer.torrent_known || !peer.torrent_capable) continue;
        for (const auto& job : peer.torrents) out.jobs.push_back({id, job});
        out.sources.push_back({id, false, peer.reachable, peer.as_of_unix_ms});
    }
    return out;
}

std::optional<ClusterTorrentJob> ClusterJobView::torrent_job(std::string_view id) const {
    if (auto local = registry_.torrent())
        if (auto job = local->job(id)) return ClusterTorrentJob{node_.node_id(), std::move(*job)};
    std::lock_guard lock(mutex_);
    for (const auto& [node, peer] : peers_)
        for (const auto& job : peer.torrents)
            if (job.id == id) return ClusterTorrentJob{node, job};
    return std::nullopt;
}

TorrentActionResult ClusterJobView::torrent_action(std::string_view id, std::string_view action) {
    TorrentActionResult result;
    if (auto local = registry_.torrent(); local && local->job(id)) {
        result.exists = true;
        if (action == "pause") result.changed = local->pause(id);
        else if (action == "resume") result.changed = local->resume(id);
        else if (action == "retry") result.changed = local->retry(id);
        else if (action == "cancel") result.changed = local->cancel(id);
        else if (action == "clear") result.changed = local->clear(id);
        if (auto updated = local->job(id)) result.updated = ClusterTorrentJob{node_.node_id(), std::move(*updated)};
        return result;
    }
    const auto owner = torrent_job(id);
    if (!owner) return result; // unknown everywhere this node has heard of
    result.exists = true;
    const auto peer = active_peer(owner->node_id);
    if (!peer) {
        result.unreachable = true;
        return result;
    }
    Json::Object request;
    request["job_id"] = std::string(id);
    request["action"] = std::string(action);
    try {
        auto reply = node_.call(*peer, MessageType::torrent_job_action, json_bytes(Json(std::move(request))),
                                FrameType::control);
        if (reply.message.type != MessageType::torrent_job_action_reply) {
            result.unreachable = true;
            return result;
        }
        auto parsed = parse_payload(reply);
        if (const auto* exists = parsed.find("exists"); exists && exists->isBool()) result.exists = exists->asBool();
        if (const auto* changed = parsed.find("changed"); changed && changed->isBool())
            result.changed = changed->asBool();
        std::optional<TorrentJob> updated;
        if (const auto* job = parsed.find("job"); job && !job->isNull()) updated = parse_torrent_job_wire(*job);
        if (updated) result.updated = ClusterTorrentJob{owner->node_id, *updated};
        // The owner's answer is the newest thing known about this job.
        std::lock_guard lock(mutex_);
        auto& jobs = peers_[owner->node_id].torrents;
        auto it = std::find_if(jobs.begin(), jobs.end(), [&](const TorrentJob& j) { return j.id == id; });
        if (it != jobs.end()) {
            if (updated) *it = *updated;
            else jobs.erase(it);
        }
    } catch (const std::exception& error) {
        Log::debug("cluster job view: torrent action to " + peer->host + ": " + error.what());
        result.unreachable = true;
    }
    return result;
}

std::vector<ClusterJobView::TorrentNode> ClusterJobView::torrent_nodes() const {
    std::vector<TorrentNode> out;
    const auto self = node_.node_id();
    if (auto local = registry_.torrent()) {
        TorrentNode entry;
        entry.node_id = self;
        entry.host = node_.config().advertise_host;
        entry.local = entry.reachable = true;
        entry.as_of_unix_ms = unix_ms();
        entry.offer = local->offer();
        entry.staging = ingest_.staging().status();
        out.push_back(std::move(entry));
    }
    std::lock_guard lock(mutex_);
    for (const auto& [id, peer] : peers_) {
        if (!peer.torrent_known || !peer.torrent_capable) continue;
        TorrentNode entry;
        entry.node_id = id;
        entry.host = peer.host;
        entry.reachable = peer.reachable;
        entry.as_of_unix_ms = peer.as_of_unix_ms;
        entry.offer = peer.offer;
        entry.staging = peer.staging;
        out.push_back(std::move(entry));
    }
    return out;
}

std::optional<std::chrono::milliseconds> ClusterJobView::default_remove_after() const {
    const auto value = default_remove_after_ms_.load(std::memory_order_relaxed);
    if (value < 0) return std::nullopt;
    return std::chrono::milliseconds(value);
}

// ---- ingest -----------------------------------------------------------------

ClusterJobView::IngestListing ClusterJobView::ingest_jobs() const {
    IngestListing out;
    const auto self = node_.node_id();
    for (auto& job : ingest_.jobs()) {
        auto summary = ingest_.catalogue_summary(job.id);
        out.jobs.push_back({self, std::move(job), std::move(summary)});
    }
    out.sources.push_back({self, true, true, unix_ms()});
    std::lock_guard lock(mutex_);
    for (const auto& [id, peer] : peers_) {
        if (!peer.ingest_known) {
            if (!peer.reachable) out.sources.push_back({id, false, false, 0});
            continue;
        }
        for (const auto& job : peer.ingests) out.jobs.push_back({id, job, {}});
        out.sources.push_back({id, false, peer.reachable, peer.as_of_unix_ms});
    }
    return out;
}

std::optional<ClusterIngestJob> ClusterJobView::ingest_job(std::string_view id) const {
    if (auto job = ingest_.job(id)) return ClusterIngestJob{node_.node_id(), std::move(*job), ingest_.catalogue_summary(id)};
    std::lock_guard lock(mutex_);
    for (const auto& [node, peer] : peers_)
        for (const auto& job : peer.ingests)
            if (job.id == id) return ClusterIngestJob{node, job, {}};
    return std::nullopt;
}

IngestActionResult ClusterJobView::ingest_action(std::string_view id, std::string_view action) {
    IngestActionResult result;
    if (ingest_.job(id)) {
        result.exists = true;
        if (action == "pause") result.changed = ingest_.pause(id);
        else if (action == "resume") result.changed = ingest_.resume(id);
        else if (action == "cancel") result.changed = ingest_.cancel(id);
        else if (action == "clear") result.changed = ingest_.clear(id);
        if (auto updated = ingest_.job(id))
            result.updated = ClusterIngestJob{node_.node_id(), std::move(*updated), ingest_.catalogue_summary(id)};
        return result;
    }
    const auto owner = ingest_job(id);
    if (!owner) return result;
    result.exists = true;
    const auto peer = active_peer(owner->node_id);
    if (!peer) {
        result.unreachable = true;
        return result;
    }
    Json::Object request;
    request["job_id"] = std::string(id);
    request["action"] = std::string(action);
    try {
        auto reply = node_.call(*peer, MessageType::ingest_job_action, json_bytes(Json(std::move(request))),
                                FrameType::control);
        if (reply.message.type != MessageType::ingest_job_action_reply) {
            result.unreachable = true;
            return result;
        }
        auto parsed = parse_payload(reply);
        if (const auto* exists = parsed.find("exists"); exists && exists->isBool()) result.exists = exists->asBool();
        if (const auto* changed = parsed.find("changed"); changed && changed->isBool())
            result.changed = changed->asBool();
        std::optional<IngestJob> updated;
        if (const auto* job = parsed.find("job"); job && !job->isNull()) updated = parse_ingest_job_wire(*job);
        if (updated) result.updated = ClusterIngestJob{owner->node_id, *updated, {}};
        std::lock_guard lock(mutex_);
        auto& jobs = peers_[owner->node_id].ingests;
        auto it = std::find_if(jobs.begin(), jobs.end(), [&](const IngestJob& j) { return j.id == id; });
        if (it != jobs.end()) {
            if (updated) *it = *updated;
            else jobs.erase(it);
        }
    } catch (const std::exception& error) {
        Log::debug("cluster job view: ingest action to " + peer->host + ": " + error.what());
        result.unreachable = true;
    }
    return result;
}

} // namespace macha

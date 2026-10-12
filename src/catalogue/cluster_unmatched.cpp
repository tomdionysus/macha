// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue/cluster_unmatched.hpp"

#include "cluster/cluster.hpp"
#include "json.hpp"
#include "log.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <random>
#include <set>

namespace macha {
namespace {

bool unmatched(const CatalogueHint& hint) {
    return hint.state == CatalogueHintState::no_match && !hint.media_id.empty();
}

Json wire_hint(const CatalogueHint& hint) {
    Json::Object out;
    out["id"] = hint.id;
    out["path"] = hint.path;
    out["provider"] = hint.provider;
    out["media_id"] = hint.media_id;
    out["result"] = hint.result;
    out["attempts"] = static_cast<uint64_t>(hint.attempts);
    out["updated_unix_ms"] = hint.updated_unix_ms;
    return Json(std::move(out));
}

std::string text_field(const Json& value, std::string_view key) {
    const auto* found = value.find(key);
    return found && found->isString() ? found->asString() : std::string{};
}

uint64_t number_field(const Json& value, std::string_view key) {
    const auto* found = value.find(key);
    return found && found->isNumber() ? found->asUInt64() : 0;
}

CatalogueHint parse_wire_hint(const Json& value) {
    CatalogueHint hint;
    hint.id = text_field(value, "id");
    hint.path = text_field(value, "path");
    hint.provider = text_field(value, "provider");
    hint.media_id = text_field(value, "media_id");
    hint.result = text_field(value, "result");
    hint.attempts = static_cast<unsigned>(number_field(value, "attempts"));
    hint.updated_unix_ms = number_field(value, "updated_unix_ms");
    hint.state = CatalogueHintState::no_match;
    return hint;
}

Bytes json_bytes(const Json& value) {
    const auto text = value.dump();
    return Bytes(text.begin(), text.end());
}

uint64_t random_instance() {
    std::random_device device;
    return (static_cast<uint64_t>(device()) << 32) | device();
}

} // namespace

ClusterUnmatchedView::ClusterUnmatchedView(NodeRuntime& node, CatalogueHintQueue& hints,
                                           std::chrono::milliseconds refresh_interval)
    : node_(node), hints_(hints), refresh_interval_(refresh_interval),
      instance_(random_instance()) {}

ClusterUnmatchedView::~ClusterUnmatchedView() { stop(); }

void ClusterUnmatchedView::start() {
    if (worker_.joinable()) return;
    worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("cluster-unmatched-view", stop, [this, stop] { loop(stop); });
    });
}

void ClusterUnmatchedView::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    wake_.notify_all();
    worker_.join();
}

void ClusterUnmatchedView::loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        refresh_now();
        Mutex wait_mutex; // guards nothing: the wait's own lock
        Lock lock(wait_mutex);
        wake_.wait_for(lock.native(), stop, refresh_interval_, [] { return false; });
    }
}

void ClusterUnmatchedView::refresh_now() {
    Lock polling(poll_mutex_);
    const auto self = node_.node_id();
    const auto telemetry = node_.telemetry().all();
    for (const auto& peer : node_.membership().active())
        if (peer.id != self && runs_at_least(telemetry, peer.id, first_version)) poll(peer);
}

void ClusterUnmatchedView::poll(const NodeInfo& info) {
    Json::Object request;
    {
        Lock lock(mutex_);
        if (auto found = peers_.find(info.id); found != peers_.end()) {
            request["instance"] = found->second.instance;
            request["revision"] = found->second.revision;
        }
    }
    try {
        // Background upkeep of a list the user reads later: loader work.
        const auto reply = node_.call(info, MessageType::get_unmatched_hints,
                                      json_bytes(Json(std::move(request))), FrameType::loader);
        if (reply.message.type != MessageType::unmatched_hints_reply) return;
        const auto parsed = Json::parse(std::string_view(
            reinterpret_cast<const char*>(reply.message.payload.data()), reply.message.payload.size()));
        Peer fresh;
        fresh.instance = number_field(parsed, "instance");
        fresh.revision = number_field(parsed, "revision");
        const auto* hints = parsed.find("hints");
        if (!hints || !hints->isArray()) return; // unchanged since the revision asked about
        for (const auto& value : hints->asArray()) {
            auto hint = parse_wire_hint(value);
            if (!hint.id.empty() && !hint.path.empty() && !hint.media_id.empty())
                fresh.hints.push_back(std::move(hint));
        }
        Lock lock(mutex_);
        peers_[info.id] = std::move(fresh);
    } catch (const std::exception& error) {
        // The peer keeps its last answer.
        Log::debug("cluster unmatched view: query to " + info.host + ": " + error.what());
    }
}

std::vector<CatalogueHint> ClusterUnmatchedView::peer_hints() const {
    std::vector<CatalogueHint> out;
    std::set<std::string, std::less<>> seen;
    Lock lock(mutex_);
    for (const auto& [_, peer] : peers_)
        for (const auto& hint : peer.hints)
            if (seen.insert(hint.id).second) out.push_back(hint);
    return out;
}

std::optional<CatalogueHint> ClusterUnmatchedView::peer_hint(std::string_view id) const {
    Lock lock(mutex_);
    for (const auto& [_, peer] : peers_)
        for (const auto& hint : peer.hints)
            if (hint.id == id) return hint;
    return std::nullopt;
}

Bytes ClusterUnmatchedView::handle_query(std::span<const uint8_t> payload) const {
    uint64_t asked_instance = 0;
    uint64_t asked_revision = 0;
    if (!payload.empty()) {
        const auto request = Json::parse(
            std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
        asked_instance = number_field(request, "instance");
        asked_revision = number_field(request, "revision");
    }
    // Read before the list, so a change made while listing is sent again.
    const auto revision = hints_.revision();
    Json::Object out;
    out["instance"] = instance_;
    out["revision"] = revision;
    if (asked_instance == instance_ && asked_revision == revision)
        return json_bytes(Json(std::move(out)));
    Json::Array hints;
    for (const auto& hint : hints_.list()) {
        if (!unmatched(hint)) continue;
        if (hints.size() == max_reply_hints) {
            Log::debug("cluster unmatched view: reply holds the first " +
                       std::to_string(max_reply_hints) + " unmatched files");
            break;
        }
        hints.push_back(wire_hint(hint));
    }
    out["hints"] = std::move(hints);
    return json_bytes(Json(std::move(out)));
}

} // namespace macha

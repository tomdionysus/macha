// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/acquisition_api.hpp"
#include "api/paging.hpp"

#include "json.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace macha {
namespace {

Json parse_body(const HttpRequest& request) {
    if (request.body.empty()) return Json(Json::Object{});
    const std::string text(reinterpret_cast<const char*>(request.body.data()), request.body.size());
    auto body = Json::parse(text);
    if (!body.isObject()) throw std::runtime_error("request body must be a JSON object");
    return body;
}

std::optional<std::pair<std::string, std::string>> job_action(std::string_view path,
                                                               std::string_view prefix) {
    if (!path.starts_with(prefix)) return {};
    path.remove_prefix(prefix.size());
    if (path.empty()) return {};
    const auto slash = path.find('/');
    if (slash == std::string_view::npos)
        return std::pair<std::string, std::string>{std::string(path), {}};
    return std::pair<std::string, std::string>{std::string(path.substr(0, slash)),
                                               std::string(path.substr(slash + 1))};
}

Json sources_json(const std::vector<ClusterJobView::Source>& sources) {
    Json::Array out;
    for (const auto& source : sources) {
        Json::Object item;
        item["node_id"] = to_string(source.node_id);
        item["local"] = source.local;
        item["reachable"] = source.reachable;
        item["as_of_unix_ms"] = source.as_of_unix_ms ? Json(source.as_of_unix_ms) : Json(nullptr);
        out.push_back(std::move(item));
    }
    return Json(std::move(out));
}

std::optional<NodeId> parse_node_id(const Json& value, std::string& error) {
    if (value.isNull()) return NodeId{};
    if (!value.isString()) {
        error = "node_id must be a string";
        return std::nullopt;
    }
    const auto bytes = unhex(value.asString());
    NodeId node;
    if (!bytes || bytes->size() != node.bytes.size()) {
        error = "node_id is not a node identifier";
        return std::nullopt;
    }
    std::copy(bytes->begin(), bytes->end(), node.bytes.begin());
    return node;
}

// remove_after_ms: absent (nullopt), null (present, empty), or 0..24 h.
bool parse_remove_after(const Json& body, std::optional<std::optional<uint64_t>>& out, std::string& error) {
    const auto* value = body.find("remove_after_ms");
    if (!value) return true;
    if (value->isNull()) {
        out = std::optional<uint64_t>{};
        return true;
    }
    if (!value->isNumber()) {
        error = "remove_after_ms must be null or a number of milliseconds";
        return false;
    }
    const auto ms = value->asInt64();
    if (ms < 0 || ms > 86400000) {
        error = "remove_after_ms must be between 0 and 86400000";
        return false;
    }
    out = std::optional<uint64_t>{static_cast<uint64_t>(ms)};
    return true;
}

HttpResponse outcome_error(const TorrentCoordinator::Outcome& outcome) {
    // This node has no namespace it can write yet; another node may.
    if (outcome.code == "metadata_unavailable") {
        FailureAxes axes;
        axes.scope = FailureScope::node;
        return http_error(outcome.status, outcome.code, outcome.message, outcome.reason, axes);
    }
    if (!outcome.reason.empty()) return http_error(outcome.status, outcome.code, outcome.message, outcome.reason);
    return http_error(outcome.status, outcome.code, outcome.message);
}

HttpResponse unreachable_error() {
    return http_error(503, "node_unreachable",
                      "the node that owns this job cannot be reached from this node");
}

HttpResponse action_error(bool exists) {
    return exists ? http_error(409, "invalid_state", "job cannot perform that action in its current state")
                  : http_error(404, "not_found", "job not found");
}

} // namespace

std::map<NodeId, uint64_t> AcquisitionApi::live_ages() const {
    std::map<NodeId, uint64_t> out;
    for (const auto& source : jobs_.torrent_jobs().sources) out[source.node_id] = source.as_of_unix_ms;
    return out;
}

// One torrent job as the API shows it: the cluster request from metadata,
// with the owner's live state laid over it.
Json AcquisitionApi::request_json(const TorrentRequest& r, const std::map<NodeId, uint64_t>& as_of) const {
    const auto live = torrents_.live_job(r);
    Json::Object out;
    if (live) {
        auto base = torrent_job_api_json(live->job);
        out = base.asObject();
    } else {
        for (const auto* key : {"bytes_total", "bytes_completed", "download_rate", "upload_rate", "uploaded_total",
                                "peers", "seeds", "eta_seconds", "progress", "catalogue", "publication",
                                "waiting_reason", "swarm"})
            out[key] = Json(nullptr);
    }
    out["id"] = r.id;
    out["info_hash"] = r.info_hash;
    out["name"] = live && !live->job.name.empty() ? live->job.name : r.name;
    out["phase"] = std::string(torrent_phase_name(r.phase));
    out["state"] = r.phase == TorrentPhase::awaiting_node ? std::string("awaiting_node")
                   : live                                ? torrent_job_state_name(live->job.state)
                                                         : std::string(torrent_phase_name(r.phase));
    out["desired"] = std::string(torrent_desired_name(r.desired));
    out["desired_changed_unix_ms"] = r.desired_changed_unix_ms;
    out["desired_applied"] = torrents_.desired_applied(r, live ? std::optional<TorrentJob>(live->job) : std::nullopt);
    const auto blocked = torrents_.desired_blocked_reason(r);
    out["desired_blocked_reason"] = blocked.empty() ? Json(nullptr) : Json(blocked);
    out["node_id"] = r.claim ? Json(to_string(r.claim->node_id)) : Json(nullptr);
    out["pinned_node_id"] = r.pinned_node_id ? Json(to_string(*r.pinned_node_id)) : Json(nullptr);
    uint64_t live_as_of = 0;
    if (live) {
        if (live->node_id == jobs_.local_node_id()) live_as_of = unix_ms();
        else if (auto found = as_of.find(live->node_id); found != as_of.end()) live_as_of = found->second;
    }
    out["live_as_of_unix_ms"] = live_as_of ? Json(live_as_of) : Json(nullptr);
    out["remove_after_ms"] = r.remove_after_ms ? Json(*r.remove_after_ms) : Json(nullptr);
    out["remove_at_unix_ms"] = r.remove_after_ms && r.completed_unix_ms
                                   ? Json(r.completed_unix_ms + *r.remove_after_ms)
                                   : Json(nullptr);
    if (!live) out["ingest_job_id"] = r.ingest_job_id.empty() ? Json(nullptr) : Json(r.ingest_job_id);
    // The ingest job is made on the node that ran the torrent.
    const auto ingest_id = out.find("ingest_job_id");
    const auto ingest_node = live ? std::optional<NodeId>(live->node_id)
                                  : r.claim ? std::optional<NodeId>(r.claim->node_id) : std::nullopt;
    out["ingest_node_id"] = ingest_id != out.end() && !ingest_id->second.isNull() && ingest_node
                                ? Json(to_string(*ingest_node))
                                : Json(nullptr);
    out["error_code"] = r.error_code.empty() ? (live ? out["error_code"] : Json(nullptr)) : Json(r.error_code);
    out["error"] = r.error.empty() ? (live ? out["error"] : Json(nullptr)) : Json(r.error);
    out["created_unix_ms"] = r.created_unix_ms;
    out["completed_unix_ms"] = r.completed_unix_ms ? Json(r.completed_unix_ms) : Json(nullptr);
    out["updated_unix_ms"] = std::max({r.progress_unix_ms, r.desired_changed_unix_ms, r.settings_changed_unix_ms,
                                       live ? live->job.updated_unix_ms : uint64_t{0}});
    return Json(std::move(out));
}

HttpResponse AcquisitionApi::handle(const HttpRequest& request) {
    try {
        if (request.method == "GET" && request.path == "/api/v1/ingest/status") {
            const auto status = ingest_.staging().status();
            Json::Object staging;
            staging["path"] = status.path.string();
            staging["limit_bytes"] = status.limit;
            staging["disk_bytes"] = status.disk_bytes;
            staging["reserved_bytes"] = status.reserved_bytes;
            staging["accounted_bytes"] = status.accounted_bytes;
            Json::Object out;
            out["enabled"] = ingest_.enabled();
            Json::Object cleanup;
            cleanup["delete_owned_source_on_clear"] = ingest_.delete_owned_source_on_clear();
            cleanup["delete_external_source_on_clear"] = ingest_.delete_external_source_on_clear();
            cleanup["delete_owned_source_on_cancel"] = ingest_.delete_owned_source_on_cancel();
            out["cleanup"] = std::move(cleanup);
            out["staging"] = std::move(staging);
            // Without these, a queue stalled behind a wedged job looks idle: every job
            // reads "queued" and nothing says whether a worker holds one.
            Json::Object concurrency;
            concurrency["max_jobs"] = static_cast<uint64_t>(ingest_.max_concurrent_jobs());
            concurrency["active_jobs"] = static_cast<uint64_t>(ingest_.active_jobs());
            concurrency["peak_active_jobs"] = static_cast<uint64_t>(ingest_.peak_active_jobs());
            out["concurrency"] = std::move(concurrency);
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "GET" && request.path == "/api/v1/ingest/jobs") {
            PageQuery page;
            if (auto bad = read_page_query(request, page)) return *bad;
            const auto listing = jobs_.ingest_jobs();
            Json::Array jobs;
            for (const auto& entry : listing.jobs) {
                auto item = ingest_job_json(entry.job, false);
                item["node_id"] = to_string(entry.node_id);
                jobs.push_back(std::move(item));
            }
            Json::Object out;
            page_json(jobs, "id", page, out);
            out["jobs"] = std::move(jobs);
            out["sources"] = sources_json(listing.sources);
            out["refresh_interval_ms"] = static_cast<uint64_t>(jobs_.refresh_interval().count());
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "POST" && request.path == "/api/v1/ingest/jobs") {
            const auto body = parse_body(request);
            const auto* path = body.find("path");
            if (!path || !path->isString() || path->asString().empty())
                return http_error(400, "bad_request", "path is required");
            std::string display_name;
            if (const auto* value = body.find("display_name")) display_name = value->asString();
            std::optional<bool> delete_source_on_clear;
            if (const auto* value = body.find("delete_source_on_clear"))
                delete_source_on_clear = value->asBool();
            else if (const auto* value = body.find("remove_source"))
                delete_source_on_clear = value->asBool(); // alias of delete_source_on_clear
            const auto id = ingest_.submit_path(path->asString(), "filesystem", {},
                                                std::move(display_name), delete_source_on_clear,
                                                false, false);
            Json::Object out;
            out["id"] = id;
            return http_json(202, Json(std::move(out)).dump());
        }

        if (auto target = job_action(request.path, "/api/v1/ingest/jobs/")) {
            if (request.method == "GET" && target->second.empty()) {
                auto existing = jobs_.ingest_job(target->first);
                if (!existing) return http_error(404, "not_found", "ingest job not found");
                auto item = ingest_job_json(existing->job, true, &existing->catalogue);
                item["node_id"] = to_string(existing->node_id);
                return http_json(200, item.dump());
            }
            if (request.method == "POST") {
                IngestActionResult result;
                if (target->second == "pause" || target->second == "resume" ||
                    target->second == "cancel" || target->second == "clear")
                    result = jobs_.ingest_action(target->first, target->second);
                else return http_error(404, "not_found", "unknown ingest action");
                if (result.unreachable) return unreachable_error();
                if (!result.changed) return action_error(result.exists);
                if (target->second == "clear") {
                    Json::Object out;
                    out["cleared"] = true;
                    return http_json(200, Json(std::move(out)).dump());
                }
                if (!result.updated) return action_error(result.exists);
                auto item = ingest_job_json(result.updated->job, false);
                item["node_id"] = to_string(result.updated->node_id);
                return http_json(200, item.dump());
            }
        }

        // The download engine is a plugin that may be absent or withdrawn while it
        // restarts; take one snapshot for this request rather than racing a restart.
        const auto torrents = subsystems_.torrent();

        if (request.method == "GET" && request.path == "/api/v1/torrents/status") {
            Json::Object out;
            out["enabled"] = torrents && torrents->enabled();
            // Whether the torrent plugin is loaded and running here.
            out["build_available"] = torrents != nullptr;
            out["search_enabled"] = search_.enabled();
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "GET" && request.path == "/api/v1/torrents/jobs") {
            // The requests from this node's metadata, with each owner's live state from
            // the cluster view.
            PageQuery page;
            if (auto bad = read_page_query(request, page)) return *bad;
            const auto listing = jobs_.torrent_jobs();
            std::map<NodeId, uint64_t> as_of;
            for (const auto& source : listing.sources) as_of[source.node_id] = source.as_of_unix_ms;
            Json::Array jobs;
            for (const auto& r : torrents_.requests()) jobs.push_back(request_json(r, as_of));
            Json::Object out;
            page_json(jobs, "id", page, out);
            out["jobs"] = std::move(jobs);
            out["sources"] = sources_json(listing.sources);
            out["refresh_interval_ms"] = static_cast<uint64_t>(jobs_.refresh_interval().count());
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "GET" && request.path == "/api/v1/torrents/nodes") {
            Json::Array nodes;
            for (const auto& node : jobs_.torrent_nodes()) {
                Json::Object item;
                item["node_id"] = to_string(node.node_id);
                item["host"] = node.host;
                item["local"] = node.local;
                item["reachable"] = node.reachable;
                item["as_of_unix_ms"] = node.as_of_unix_ms ? Json(node.as_of_unix_ms) : Json(nullptr);
                item["accepting"] = node.offer.accepting && node.reachable;
                item["not_accepting_reason"] =
                    !node.reachable ? Json(std::string("unreachable"))
                    : node.offer.not_accepting_reason.empty() ? Json(nullptr)
                                                              : Json(node.offer.not_accepting_reason);
                item["max_active"] = static_cast<uint64_t>(node.offer.max_active);
                item["active_jobs"] = static_cast<uint64_t>(node.offer.active_jobs);
                item["staging"] = node.staging ? staging_capacity_json(*node.staging) : Json(nullptr);
                nodes.push_back(std::move(item));
            }
            Json::Object out;
            out["nodes"] = std::move(nodes);
            out["refresh_interval_ms"] = static_cast<uint64_t>(jobs_.refresh_interval().count());
            const auto remove_after = jobs_.default_remove_after();
            out["default_remove_after_ms"] =
                remove_after ? Json(static_cast<uint64_t>(remove_after->count())) : Json(nullptr);
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "POST" && request.path == "/api/v1/torrents/jobs") {
            const auto body = parse_body(request);
            // Optional pin: absent or null lets the cluster choose.
            std::optional<NodeId> pin;
            if (const auto* node = body.find("node_id")) {
                std::string error;
                auto parsed = parse_node_id(*node, error);
                if (!parsed) return http_error(400, "bad_request", error);
                if (*parsed != NodeId{}) pin = *parsed;
            }
            std::optional<std::optional<uint64_t>> remove_after;
            if (std::string error; !parse_remove_after(body, remove_after, error))
                return http_error(400, "bad_request", error);
            // Optional: created already paused, in the same write.
            bool paused = false;
            if (const auto* value = body.find("paused"); value && !value->isNull()) {
                if (!value->isBool()) return http_error(400, "bad_request", "paused must be a boolean");
                paused = value->asBool();
            }

            std::string uri;
            bool search_result = false;
            if (const auto* magnet = body.find("magnet"); magnet && magnet->isString()) {
                uri = magnet->asString();
            } else if (const auto* ref = body.find("acquisition_ref"); ref && ref->isString()) {
                auto resolved = search_.resolve(ref->asString());
                if (!resolved)
                    return http_error(404, "not_found",
                                      "torrent search result expired or not found");
                uri = std::move(*resolved);
                search_result = true;
            } else {
                return http_error(400, "bad_request", "magnet or acquisition_ref is required");
            }

            const auto outcome = torrents_.add(uri, search_result, pin, remove_after, paused);
            if (outcome.code == "torrent_already_added") {
                // The holder is named in the same fields a 202 uses, so a
                // client can go straight to it.
                Json::Object root;
                root["status"] = std::string("torrent_already_added");
                root["error"] = Json::Object{{"code", std::string("torrent_already_added")},
                                             {"message", outcome.message}};
                root["id"] = outcome.request ? Json(outcome.request->id) : Json(nullptr);
                root["node_id"] = outcome.node ? Json(to_string(*outcome.node)) : Json(nullptr);
                return http_json(409, Json(std::move(root)).dump());
            }
            if (outcome.status != 202) return outcome_error(outcome);
            Json::Object out;
            out["id"] = outcome.request->id;
            out["info_hash"] = outcome.request->info_hash;
            out["node_id"] = pin ? Json(to_string(*pin)) : Json(nullptr);
            out["job"] = request_json(*outcome.request, {});
            return http_json(202, Json(std::move(out)).dump());
        }

        if (request.method == "GET" && request.path == "/api/v1/torrents/search") {
            const auto it = request.query.find("q");
            if (it == request.query.end() || it->second.empty())
                return http_error(400, "bad_request", "q is required");
            auto response = search_.search(it->second);
            Json::Array results;
            for (const auto& result : response.results) {
                if (result.acquisition_ref.empty()) continue;
                Json::Object item;
                item["acquisition_ref"] = result.acquisition_ref;
                item["provider"] = result.provider;
                item["title"] = result.title;
                item["size_bytes"] = optional_u64(result.size_bytes);
                item["seeders"] = optional_u64(result.seeders);
                item["leechers"] = optional_u64(result.leechers);
                item["published"] = result.published.empty() ? Json(nullptr) : Json(result.published);
                results.emplace_back(std::move(item));
            }
            Json::Object errors;
            for (const auto& [provider, message] : response.errors) errors[provider] = message;
            Json::Object out;
            out["results"] = std::move(results);
            out["provider_errors"] = std::move(errors);
            return http_json(200, Json(std::move(out)).dump());
        }

        if (auto target = job_action(request.path, "/api/v1/torrents/jobs/")) {
            if (request.method == "GET" && target->second.empty()) {
                auto existing = torrents_.request(target->first);
                if (!existing) return http_error(404, "not_found", "torrent job not found");
                return http_json(200, request_json(*existing, live_ages()).dump());
            }
            if (request.method == "PATCH" && target->second.empty()) {
                const auto body = parse_body(request);
                std::optional<std::optional<uint64_t>> remove_after;
                if (std::string error; !parse_remove_after(body, remove_after, error))
                    return http_error(400, "bad_request", error);
                std::optional<std::optional<NodeId>> pin;
                if (const auto* node = body.find("node_id")) {
                    std::string error;
                    auto parsed = parse_node_id(*node, error);
                    if (!parsed) return http_error(400, "bad_request", error);
                    pin = *parsed == NodeId{} ? std::optional<NodeId>{} : std::optional<NodeId>{*parsed};
                }
                if (!remove_after && !pin)
                    return http_error(400, "bad_request", "remove_after_ms or node_id is required");
                const auto outcome = torrents_.patch(target->first, remove_after, pin);
                if (outcome.status != 200) return outcome_error(outcome);
                return http_json(200, request_json(*outcome.request, live_ages()).dump());
            }
            if (request.method == "POST") {
                const auto& action = target->second;
                if (action != "pause" && action != "resume" && action != "retry" && action != "cancel" &&
                    action != "clear")
                    return http_error(404, "not_found", "unknown torrent action");
                const auto outcome = torrents_.act(target->first, action);
                if (outcome.status != 202) return outcome_error(outcome);
                if (action == "clear") {
                    Json::Object out;
                    out["cleared"] = true;
                    return http_json(202, Json(std::move(out)).dump());
                }
                // Intent recorded; the owner applies it and `state` follows.
                const auto shown = outcome.request ? outcome.request : torrents_.request(target->first);
                if (!shown) return http_error(404, "not_found", "torrent job not found");
                return http_json(202, request_json(*shown, live_ages()).dump());
            }
        }

        return http_error(404, "not_found", "acquisition endpoint not found");
    } catch (const JsonError& e) {
        return http_error(400, "bad_json", e.what());
    } catch (const std::exception& e) {
        return http_error(400, "bad_request", e.what());
    }
}

} // namespace macha

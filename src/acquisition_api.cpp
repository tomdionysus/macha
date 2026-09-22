// SPDX-License-Identifier: GPL-3.0-or-later
#include "acquisition_api.hpp"

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

HttpResponse action_error(bool exists) {
    return exists ? http_error(409, "invalid_state", "job cannot perform that action in its current state")
                  : http_error(404, "not_found", "job not found");
}

} // namespace

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
            // Without these, a queue stalled behind a wedged job is
            // indistinguishable from an idle one: every job reads "queued"
            // and nothing says whether a worker is holding any of them.
            Json::Object concurrency;
            concurrency["max_jobs"] = static_cast<uint64_t>(ingest_.max_concurrent_jobs());
            concurrency["active_jobs"] = static_cast<uint64_t>(ingest_.active_jobs());
            concurrency["peak_active_jobs"] = static_cast<uint64_t>(ingest_.peak_active_jobs());
            out["concurrency"] = std::move(concurrency);
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "GET" && request.path == "/api/v1/ingest/jobs") {
            Json::Array jobs;
            for (const auto& entry : ingest_.jobs_cluster_wide()) {
                auto item = ingest_job_json(entry.job, false);
                item["node_id"] = to_string(entry.node_id);
                jobs.push_back(std::move(item));
            }
            Json::Object out;
            out["jobs"] = std::move(jobs);
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
                delete_source_on_clear = value->asBool(); // 0.13 compatibility alias
            const auto id = ingest_.submit_path(path->asString(), "filesystem", {},
                                                std::move(display_name), delete_source_on_clear,
                                                false, false);
            Json::Object out;
            out["id"] = id;
            return http_json(202, Json(std::move(out)).dump());
        }

        if (auto target = job_action(request.path, "/api/v1/ingest/jobs/")) {
            if (request.method == "GET" && target->second.empty()) {
                auto existing = ingest_.job_cluster_wide(target->first);
                if (!existing) return http_error(404, "not_found", "ingest job not found");
                auto item = ingest_job_json(existing->job, true, &existing->catalogue);
                item["node_id"] = to_string(existing->node_id);
                return http_json(200, item.dump());
            }
            if (request.method == "POST") {
                IngestActionResult result;
                if (target->second == "pause") result = ingest_.pause_cluster_wide(target->first);
                else if (target->second == "resume") result = ingest_.resume_cluster_wide(target->first);
                else if (target->second == "cancel") result = ingest_.cancel_cluster_wide(target->first);
                else if (target->second == "clear") result = ingest_.clear_cluster_wide(target->first);
                else return http_error(404, "not_found", "unknown ingest action");
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

        // The download engine lives in the libmacha-torrent plugin, so its
        // presence is a runtime fact, per node, that can also change while
        // the process runs (a faulted subsystem is withdrawn until it
        // restarts). Take one snapshot for this request rather than looking
        // it up repeatedly and racing with a restart mid-handler.
        const auto torrents = subsystems_.torrent();

        if (request.method == "GET" && request.path == "/api/v1/torrents/status") {
            Json::Object out;
            out["enabled"] = torrents && torrents->enabled();
            // Retains the pre-0.28.0 field name: it used to report whether
            // libtorrent was compiled in, and now reports whether the plugin
            // providing it is loaded and running here -- the same question a
            // client was asking, answered at runtime.
            out["build_available"] = torrents != nullptr;
            out["search_enabled"] = search_.enabled();
            return http_json(200, Json(std::move(out)).dump());
        }

        // Search is served by core (a Torznab HTTP client, no libtorrent), so
        // it keeps working on a node with no plugin installed; everything
        // else under /torrents/ needs the engine itself.
        if (!torrents && request.path.starts_with("/api/v1/torrents/") &&
            request.path != "/api/v1/torrents/search")
            return http_error(503, "unavailable",
                              "torrent subsystem is not available on this node");

        if (request.method == "GET" && request.path == "/api/v1/torrents/jobs") {
            Json::Array jobs;
            for (const auto& entry : torrents->jobs_cluster_wide()) {
                auto item = torrent_job_api_json(entry.job);
                item["node_id"] = to_string(entry.node_id);
                jobs.push_back(std::move(item));
            }
            Json::Object out;
            out["jobs"] = std::move(jobs);
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "POST" && request.path == "/api/v1/torrents/jobs") {
            const auto body = parse_body(request);

            // Optional placement. Absent means "here", which is what this route
            // always did; a node id means that node downloads it. Which node
            // runs a download is not a detail -- they differ in disk, in memory
            // and in what else they are serving -- and until now it was decided
            // by which address the client happened to be configured with.
            NodeId target{};
            if (const auto* node = body.find("node_id"); node && !node->isNull()) {
                if (!node->isString())
                    return http_error(400, "bad_request", "node_id must be a string");
                const auto bytes = unhex(node->asString());
                if (!bytes || bytes->size() != NodeId{}.bytes.size())
                    return http_error(400, "bad_request", "node_id is not a node identifier");
                std::copy(bytes->begin(), bytes->end(), target.bytes.begin());
            }

            std::string uri;
            if (const auto* magnet = body.find("magnet"); magnet && magnet->isString()) {
                uri = magnet->asString();
            } else if (const auto* ref = body.find("acquisition_ref"); ref && ref->isString()) {
                auto resolved = search_.resolve(ref->asString());
                if (!resolved)
                    return http_error(404, "not_found",
                                      "torrent search result expired or not found");
                uri = std::move(*resolved);
            } else {
                return http_error(400, "bad_request", "magnet or acquisition_ref is required");
            }

            const auto placement = torrents->add_on(target, uri);
            if (!placement.placed) {
                // An unreachable or unknown target is refused rather than
                // quietly downloaded here. A job that lands somewhere the
                // operator did not ask for is worse than one that fails.
                return http_error(409, "placement_failed",
                                  placement.error.empty() ? "the job could not be placed"
                                                          : placement.error);
            }
            Json::Object out;
            out["id"] = placement.job_id;
            // Always reported, including for a local add, so a client never has
            // to infer where its own job went.
            out["node_id"] = to_string(placement.node_id);
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
                auto existing = torrents->job_cluster_wide(target->first);
                if (!existing) return http_error(404, "not_found", "torrent job not found");
                auto item = torrent_job_api_json(existing->job);
                item["node_id"] = to_string(existing->node_id);
                return http_json(200, item.dump());
            }
            if (request.method == "POST") {
                TorrentActionResult result;
                if (target->second == "pause") result = torrents->pause_cluster_wide(target->first);
                else if (target->second == "resume") result = torrents->resume_cluster_wide(target->first);
                else if (target->second == "retry") result = torrents->retry_cluster_wide(target->first);
                else if (target->second == "cancel") result = torrents->cancel_cluster_wide(target->first);
                else if (target->second == "clear") result = torrents->clear_cluster_wide(target->first);
                else return http_error(404, "not_found", "unknown torrent action");
                if (!result.changed) return action_error(result.exists);
                if (target->second == "clear") {
                    Json::Object out;
                    out["cleared"] = true;
                    return http_json(200, Json(std::move(out)).dump());
                }
                if (!result.updated) return action_error(result.exists);
                auto item = torrent_job_api_json(result.updated->job);
                item["node_id"] = to_string(result.updated->node_id);
                return http_json(200, item.dump());
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

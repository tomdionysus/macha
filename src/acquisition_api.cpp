// SPDX-License-Identifier: GPL-3.0-or-later
#include "acquisition_api.hpp"

#include "json.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

namespace macha {
namespace {

Json optional_u64(const std::optional<uint64_t>& value) {
    return value ? Json(*value) : Json(nullptr);
}

Json catalogue_summary_json(const IngestJob& job, const CatalogueHintSummary* detail = nullptr) {
    Json::Object out;
    out["total"] = static_cast<uint64_t>(job.catalogue_total);
    out["pending"] = static_cast<uint64_t>(job.catalogue_pending);
    out["catalogued"] = static_cast<uint64_t>(job.catalogue_catalogued);
    out["no_match"] = static_cast<uint64_t>(job.catalogue_no_match);
    out["failed"] = static_cast<uint64_t>(job.catalogue_failed);
    if (job.state == IngestJobState::cataloguing || job.catalogue_pending)
        out["state"] = "processing";
    else if (!job.catalogue_total && job.state != IngestJobState::completed)
        out["state"] = "waiting";
    else if (job.catalogue_failed || job.catalogue_no_match)
        out["state"] = "completed_with_issues";
    else
        out["state"] = "completed";

    if (detail) {
        Json::Array items;
        items.reserve(detail->hints.size());
        for (const auto& hint : detail->hints) {
            Json::Object item;
            item["id"] = hint.id;
            item["path"] = hint.path;
            item["state"] = catalogue_hint_state_name(hint.state);
            item["provider"] = hint.provider.empty() ? Json(nullptr) : Json(hint.provider);
            item["media_id"] = hint.media_id.empty() ? Json(nullptr) : Json(hint.media_id);
            item["priority"] = static_cast<int64_t>(hint.priority);
            item["attempts"] = static_cast<uint64_t>(hint.attempts);
            item["result"] = hint.result.empty() ? Json(nullptr) : Json(hint.result);
            item["error"] = hint.error.empty() ? Json(nullptr) : Json(hint.error);
            Json::Array ids;
            for (const auto& id : hint.catalogue_item_ids) ids.emplace_back(id);
            item["catalogue_item_ids"] = std::move(ids);
            items.emplace_back(std::move(item));
        }
        out["items"] = std::move(items);
    }
    return Json(std::move(out));
}

Json ingest_job_json(const IngestJob& job, bool include_files,
                     const CatalogueHintSummary* catalogue_detail = nullptr) {
    Json::Object out;
    out["id"] = job.id;
    out["source_type"] = job.source_type;
    out["source_ref"] = job.source_ref.empty() ? Json(nullptr) : Json(job.source_ref);
    out["display_name"] = job.display_name;
    out["source_path"] = job.source_path.string();
    out["source_owned"] = job.source_owned;
    out["delete_source_on_clear"] = job.delete_source_on_clear;
    out["state"] = ingest_job_state_name(job.state);
    out["bytes_total"] = job.bytes_total;
    out["bytes_completed"] = job.bytes_completed;
    out["files_total"] = static_cast<uint64_t>(job.files_total);
    out["files_completed"] = static_cast<uint64_t>(job.files_completed);
    out["rate_bytes_per_second"] = job.rate_bytes_per_second;
    out["eta_seconds"] = optional_u64(job.eta_seconds);
    out["progress"] = job.bytes_total
                          ? Json(std::min(1.0, static_cast<double>(job.bytes_completed) /
                                                   static_cast<double>(job.bytes_total)))
                          : Json(nullptr);
    out["current_file"] = job.current_file.empty() ? Json(nullptr) : Json(job.current_file);
    out["current_destination"] =
        job.current_destination.empty() ? Json(nullptr) : Json(job.current_destination);
    out["catalogue"] = catalogue_summary_json(job, catalogue_detail);
    out["created_unix_ms"] = job.created_unix_ms;
    out["updated_unix_ms"] = job.updated_unix_ms;
    out["error"] = job.error.empty() ? Json(nullptr) : Json(job.error);

    if (include_files) {
        Json::Array files;
        files.reserve(job.files.size());
        for (const auto& file : job.files) {
            Json::Object item;
            item["source_path"] = file.source_path;
            item["destination_path"] = file.destination_path;
            item["size"] = file.size;
            item["copied"] = file.copied;
            item["completed"] = file.completed;
            item["skipped"] = file.skipped;
            item["catalogue_candidate"] = file.catalogue_candidate;
            files.emplace_back(std::move(item));
        }
        out["files"] = std::move(files);
    }
    return Json(std::move(out));
}

Json torrent_job_json(const TorrentJob& job) {
    Json::Object out;
    out["id"] = job.id;
    out["name"] = job.name;
    out["info_hash"] = job.info_hash.empty() ? Json(nullptr) : Json(job.info_hash);
    out["state"] = torrent_job_state_name(job.state);
    out["bytes_total"] = job.bytes_total;
    out["bytes_completed"] = job.bytes_completed;
    out["download_rate"] = job.download_rate;
    out["upload_rate"] = job.upload_rate;
    out["uploaded_total"] = job.uploaded_total;
    out["peers"] = static_cast<uint64_t>(job.peers);
    out["seeds"] = static_cast<uint64_t>(job.seeds);
    Json::Object catalogue;
    catalogue["total"] = static_cast<uint64_t>(job.catalogue_total);
    catalogue["pending"] = static_cast<uint64_t>(job.catalogue_pending);
    catalogue["catalogued"] = static_cast<uint64_t>(job.catalogue_catalogued);
    catalogue["no_match"] = static_cast<uint64_t>(job.catalogue_no_match);
    catalogue["failed"] = static_cast<uint64_t>(job.catalogue_failed);
    if (job.catalogue_pending)
        catalogue["state"] = "processing";
    else if (job.catalogue_total && (job.catalogue_failed || job.catalogue_no_match))
        catalogue["state"] = "completed_with_issues";
    else if (job.catalogue_total)
        catalogue["state"] = "completed";
    else
        catalogue["state"] = "waiting";
    out["catalogue"] = std::move(catalogue);
    out["eta_seconds"] = optional_u64(job.eta_seconds);
    out["progress"] = job.bytes_total
                          ? Json(std::min(1.0, static_cast<double>(job.bytes_completed) /
                                                   static_cast<double>(job.bytes_total)))
                          : Json(nullptr);
    out["ingest_job_id"] = job.ingest_job_id ? Json(*job.ingest_job_id) : Json(nullptr);
    out["created_unix_ms"] = job.created_unix_ms;
    out["updated_unix_ms"] = job.updated_unix_ms;
    out["error"] = job.error.empty() ? Json(nullptr) : Json(job.error);
    return Json(std::move(out));
}

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
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "GET" && request.path == "/api/v1/ingest/jobs") {
            Json::Array jobs;
            for (const auto& job : ingest_.jobs()) jobs.push_back(ingest_job_json(job, false));
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
            const auto existing = ingest_.job(target->first);
            if (request.method == "GET" && target->second.empty()) {
                if (!existing) return http_error(404, "not_found", "ingest job not found");
                const auto summary = ingest_.catalogue_summary(target->first);
                return http_json(200, ingest_job_json(*existing, true, &summary).dump());
            }
            if (request.method == "POST") {
                bool changed = false;
                if (target->second == "pause") changed = ingest_.pause(target->first);
                else if (target->second == "resume") changed = ingest_.resume(target->first);
                else if (target->second == "cancel") changed = ingest_.cancel(target->first);
                else if (target->second == "clear") changed = ingest_.clear(target->first);
                else return http_error(404, "not_found", "unknown ingest action");
                if (!changed) return action_error(existing.has_value());
                if (target->second == "clear") {
                    Json::Object out;
                    out["cleared"] = true;
                    return http_json(200, Json(std::move(out)).dump());
                }
                auto updated = ingest_.job(target->first);
                return http_json(200, ingest_job_json(*updated, false).dump());
            }
        }

        if (request.method == "GET" && request.path == "/api/v1/torrents/status") {
            Json::Object out;
            out["enabled"] = torrents_.enabled();
            out["build_available"] = TorrentManager::build_available();
            out["search_enabled"] = search_.enabled();
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "GET" && request.path == "/api/v1/torrents/jobs") {
            Json::Array jobs;
            for (const auto& job : torrents_.jobs()) jobs.push_back(torrent_job_json(job));
            Json::Object out;
            out["jobs"] = std::move(jobs);
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "POST" && request.path == "/api/v1/torrents/jobs") {
            const auto body = parse_body(request);
            std::string id;
            if (const auto* magnet = body.find("magnet"); magnet && magnet->isString()) {
                id = torrents_.add(magnet->asString());
            } else if (const auto* ref = body.find("acquisition_ref"); ref && ref->isString()) {
                auto uri = search_.resolve(ref->asString());
                if (!uri) return http_error(404, "not_found", "torrent search result expired or not found");
                id = torrents_.add_search_result(std::move(*uri));
            } else {
                return http_error(400, "bad_request", "magnet or acquisition_ref is required");
            }
            Json::Object out;
            out["id"] = id;
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
            const auto existing = torrents_.job(target->first);
            if (request.method == "GET" && target->second.empty()) {
                if (!existing) return http_error(404, "not_found", "torrent job not found");
                return http_json(200, torrent_job_json(*existing).dump());
            }
            if (request.method == "POST") {
                bool changed = false;
                if (target->second == "pause") changed = torrents_.pause(target->first);
                else if (target->second == "resume") changed = torrents_.resume(target->first);
                else if (target->second == "cancel") changed = torrents_.cancel(target->first);
                else if (target->second == "clear") changed = torrents_.clear(target->first);
                else return http_error(404, "not_found", "unknown torrent action");
                if (!changed) return action_error(existing.has_value());
                if (target->second == "clear") {
                    Json::Object out;
                    out["cleared"] = true;
                    return http_json(200, Json(std::move(out)).dump());
                }
                auto updated = torrents_.job(target->first);
                return http_json(200, torrent_job_json(*updated).dump());
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

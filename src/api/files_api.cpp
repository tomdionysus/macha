// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/files_api.hpp"

#include <cerrno>

namespace macha {

namespace {

constexpr std::string_view prefix = "/api/v1/files";

std::string child_path(const std::string& directory, const std::string& name) {
    return directory == "/" ? "/" + name : directory + "/" + name;
}

const PathAvailability* facts_of(const AvailabilitySnapshot* snapshot, std::string_view path) {
    if (!snapshot)
        return nullptr;
    const auto found = snapshot->paths.find(path);
    return found == snapshot->paths.end() ? nullptr : &found->second;
}

Json entry_json(const std::string& path, const FsEntry& entry,
                const AvailabilitySnapshot* snapshot) {
    Json::Object out;
    out["path"] = path;
    const auto slash = path.rfind('/');
    out["name"] = path == "/" ? std::string() : path.substr(slash + 1);
    const bool directory = entry.type == EntryType::directory;
    out["type"] = directory ? "directory" : "file";
    out["mtime_ns"] = entry.mtime_ns;
    auto facts = facts_of(snapshot, path);
    // A file written since the survey is not the file that was surveyed.
    if (facts && (facts->directory != directory || (!directory && facts->size != entry.size)))
        facts = nullptr;
    out["size"] = directory ? (facts ? facts->size : 0) : entry.size;
    out["media_id"] = facts && !facts->hash.empty() ? Json(facts->hash) : Json(nullptr);
    put_availability(out, snapshot, facts);
    return Json(std::move(out));
}

} // namespace

void put_availability(Json::Object& out, const AvailabilitySnapshot* snapshot,
                      const PathAvailability* facts) {
    if (!facts) {
        out["extents"] = Json(nullptr);
        out["extents_local"] = Json(nullptr);
        out["extents_unavailable"] = Json(nullptr);
        out["extents_unknown"] = Json(nullptr);
        out["availability"] = "unknown";
    } else {
        out["extents"] = facts->extents;
        out["extents_local"] = facts->extents_local;
        out["extents_unavailable"] = facts->extents_unavailable;
        out["extents_unknown"] = facts->extents_unknown;
        out["availability"] = facts->extents_unavailable ? "partial"
                              : facts->extents_unknown   ? "unknown"
                                                         : "complete";
    }
    out["surveyed_generation"] = snapshot ? Json(snapshot->generation) : Json(nullptr);
    out["surveyed_unix_ms"] = snapshot ? Json(snapshot->surveyed_unix_ms) : Json(nullptr);
}

void put_media_availability(Json::Object& out, const AvailabilitySnapshot* snapshot,
                            std::string_view media_id) {
    const PathAvailability* facts = nullptr;
    if (snapshot)
        if (const auto found = snapshot->by_hash.find(media_id); found != snapshot->by_hash.end())
            facts = facts_of(snapshot, found->second);
    put_availability(out, snapshot, facts);
}

FilesApi::FilesApi(FileSystem& fs, const AvailabilityService& availability)
    : fs_(fs), availability_(availability) {}

HttpResponse FilesApi::handle(const HttpRequest& request) {
    if (request.method != "GET")
        return http_error(405, "method_not_allowed", "the files resource is read with GET");
    const auto snapshot = availability_.snapshot();

    if (const auto hash = request.query.find("hash"); hash != request.query.end()) {
        if (request.path != prefix && request.path != std::string(prefix) + "/")
            return http_error(400, "bad_request", "hash filters the files collection itself");
        if (!hash->second.starts_with("macha:"))
            return http_error(400, "bad_media_id", "immutable macha media ID required");
        Json::Array files;
        if (snapshot) {
            const auto [first, last] = snapshot->by_hash.equal_range(hash->second);
            for (auto it = first; it != last; ++it) {
                const auto* facts = facts_of(snapshot.get(), it->second);
                if (!facts)
                    continue;
                Json::Object file;
                file["path"] = it->second;
                file["name"] = it->second.substr(it->second.rfind('/') + 1);
                file["type"] = "file";
                file["size"] = facts->size;
                file["media_id"] = facts->hash;
                put_availability(file, snapshot.get(), facts);
                files.emplace_back(std::move(file));
            }
        }
        Json::Object out;
        out["status"] = "ok";
        out["files"] = std::move(files);
        out["surveyed_generation"] = snapshot ? Json(snapshot->generation) : Json(nullptr);
        out["surveyed_unix_ms"] = snapshot ? Json(snapshot->surveyed_unix_ms) : Json(nullptr);
        return http_json(200, Json(std::move(out)).dump());
    }

    auto path = request.path.substr(prefix.size());
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    if (path.empty())
        path = "/";
    try {
        const auto entry = fs_.getattr(path);
        auto out = entry_json(path, entry, snapshot.get()).asObject();
        out["status"] = "ok";
        if (entry.type == EntryType::directory) {
            Json::Array entries;
            for (const auto& [name, child] : fs_.readdir(path))
                entries.push_back(entry_json(child_path(path, name), child, snapshot.get()));
            out["entries"] = std::move(entries);
        }
        return http_json(200, Json(std::move(out)).dump());
    } catch (const FsError& error) {
        if (error.code() == ENOENT || error.code() == ENOTDIR)
            return http_error(404, "not_found", "no such file or directory");
        throw;
    }
}

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#include "manage_api.hpp"

#include "crypto.hpp"
#include "json.hpp"
#include "log.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <set>
#include <sstream>
#include <unistd.h>

namespace macha {
namespace {

Json parse_body(const HttpRequest& request) {
    const std::string text(reinterpret_cast<const char*>(request.body.data()), request.body.size());
    if (text.empty()) return Json(Json::Object{});
    auto root = Json::parse(text);
    if (!root.isObject()) throw std::runtime_error("request body must be a JSON object");
    return root;
}

std::string string_value(const Json& root, std::string_view key, std::string fallback = {}) {
    const auto* value = root.find(key);
    if (!value || value->isNull()) return fallback;
    return value->asString();
}

std::string required_string(const Json& root, std::string_view key) {
    auto value = string_value(root, key);
    if (value.empty()) throw std::runtime_error(std::string(key) + " is required");
    return value;
}

std::optional<int32_t> optional_i32(const Json& root, std::string_view key) {
    const auto* value = root.find(key);
    if (!value || value->isNull()) return {};
    const auto number = value->asInt64();
    if (number < INT32_MIN || number > INT32_MAX)
        throw std::runtime_error(std::string(key) + " is out of range");
    return static_cast<int32_t>(number);
}

bool bool_value(const Json& root, std::string_view key, bool fallback) {
    const auto* value = root.find(key);
    return value && !value->isNull() ? value->asBool() : fallback;
}

Json artwork_json(const CatalogueArtwork& art) {
    Json::Object out;
    out["role"] = art.role;
    out["id"] = to_string(art.id);
    out["mime_type"] = art.mime_type;
    return Json(std::move(out));
}

Json catalogue_item_json(const CatalogueItem& item) {
    Json::Object out;
    out["id"] = item.id;
    out["kind"] = catalogue_kind_name(item.kind);
    out["title"] = item.title;
    out["sort_title"] = item.sort_title;
    out["synopsis"] = item.synopsis;
    out["parent_id"] = item.parent_id ? Json(*item.parent_id) : Json(nullptr);
    out["year"] = item.year ? Json(static_cast<int64_t>(*item.year)) : Json(nullptr);
    out["season_number"] = item.season_number ? Json(static_cast<int64_t>(*item.season_number)) : Json(nullptr);
    out["episode_number"] = item.episode_number ? Json(static_cast<int64_t>(*item.episode_number)) : Json(nullptr);
    out["disc_number"] = item.disc_number ? Json(static_cast<int64_t>(*item.disc_number)) : Json(nullptr);
    out["track_number"] = item.track_number ? Json(static_cast<int64_t>(*item.track_number)) : Json(nullptr);
    Json::Array media;
    for (const auto& id : item.media_ids) media.emplace_back(id);
    out["media_ids"] = std::move(media);
    Json::Array artwork;
    for (const auto& art : item.artwork) artwork.push_back(artwork_json(art));
    out["artwork"] = std::move(artwork);
    out["revision"] = item.revision;
    out["updated_ns"] = item.updated_ns;
    return Json(std::move(out));
}

std::string entry_path(std::string_view parent, std::string_view name) {
    if (parent == "/") return normalize_path("/" + std::string(name));
    return normalize_path(std::string(parent) + "/" + std::string(name));
}

Json fs_entry_json(std::string path, std::string name, const FsEntry& entry,
                   const std::map<std::string, std::vector<std::string>, std::less<>>& bindings) {
    Json::Object out;
    out["path"] = std::move(path);
    out["name"] = std::move(name);
    out["type"] = entry.type == EntryType::directory ? "directory" : "file";
    out["size"] = entry.size;
    out["mtime_ns"] = entry.mtime_ns;
    out["mode"] = static_cast<uint64_t>(entry.mode);
    if (entry.type == EntryType::file && entry.size != 0) {
        const auto media_id = file_media_id(entry);
        out["media_id"] = media_id;
        Json::Array ids;
        if (auto found = bindings.find(media_id); found != bindings.end())
            for (const auto& id : found->second) ids.emplace_back(id);
        out["catalogue_item_ids"] = std::move(ids);
    } else {
        out["media_id"] = Json(nullptr);
        out["catalogue_item_ids"] = Json::Array{};
    }
    return Json(std::move(out));
}

std::map<std::string, std::vector<std::string>, std::less<>> media_bindings(
    const CatalogueSnapshot& snapshot) {
    std::map<std::string, std::vector<std::string>, std::less<>> out;
    for (const auto& [id, item] : snapshot.items)
        for (const auto& media_id : item.media_ids)
            out[media_id].push_back(id);
    return out;
}

std::optional<std::pair<FsEntry, std::string>> current_hint_file(
    FileSystem& fs, const CatalogueHint& hint) {
    try {
        auto entry = fs.getattr(hint.path);
        if (entry.type != EntryType::file || entry.size == 0) return {};
        auto media_id = file_media_id(entry);
        if (!hint.media_id.empty() && hint.media_id != media_id) return {};
        return std::pair<FsEntry, std::string>{std::move(entry), std::move(media_id)};
    } catch (const FsError&) {
        return {};
    }
}

Json hint_json(FileSystem& fs, const CatalogueHint& hint) {
    Json::Object out;
    out["id"] = hint.id;
    out["path"] = hint.path;
    out["provider"] = hint.provider.empty() ? Json(nullptr) : Json(hint.provider);
    out["media_id"] = hint.media_id.empty() ? Json(nullptr) : Json(hint.media_id);
    out["result"] = hint.result;
    out["attempts"] = static_cast<uint64_t>(hint.attempts);
    out["updated_unix_ms"] = hint.updated_unix_ms;
    if (auto current = current_hint_file(fs, hint)) {
        out["size"] = current->first.size;
        out["mtime_ns"] = current->first.mtime_ns;
        out["current"] = true;
    } else {
        out["size"] = static_cast<uint64_t>(0);
        out["mtime_ns"] = static_cast<int64_t>(0);
        out["current"] = false;
    }
    return Json(std::move(out));
}

Json probe_json(const MediaProbeCandidate& candidate) {
    Json::Object out;
    const auto& probe = candidate.probe;
    out["kind"] = probe.kind == MediaProbeKind::movie ? "movie" :
                  probe.kind == MediaProbeKind::episode ? "episode" : "track";
    out["score"] = static_cast<int64_t>(candidate.score);
    out["generator"] = candidate.generator;
    out["title"] = probe.title;
    out["year"] = probe.year ? Json(static_cast<int64_t>(*probe.year)) : Json(nullptr);
    out["series"] = probe.series;
    out["season_number"] = probe.season ? Json(static_cast<int64_t>(*probe.season)) : Json(nullptr);
    out["episode_number"] = probe.episode ? Json(static_cast<int64_t>(*probe.episode)) : Json(nullptr);
    out["artist"] = probe.artist;
    out["album"] = probe.album;
    out["disc_number"] = probe.disc ? Json(static_cast<int64_t>(*probe.disc)) : Json(nullptr);
    out["track_number"] = probe.track ? Json(static_cast<int64_t>(*probe.track)) : Json(nullptr);
    Json::Array evidence;
    for (const auto& value : candidate.evidence) evidence.emplace_back(value);
    out["evidence"] = std::move(evidence);
    return Json(std::move(out));
}

std::string filename_query(std::string_view path) {
    auto slash = path.find_last_of('/');
    auto name = std::string(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
    if (auto dot = name.find_last_of('.'); dot != std::string::npos) name.resize(dot);
    for (auto& c : name)
        if (c == '.' || c == '_' || c == '-') c = ' ';
    return name;
}

std::optional<CatalogueKind> leaf_kind(const std::vector<MediaProbeCandidate>& probes,
                                       std::string_view provider) {
    if (!probes.empty()) {
        switch (probes.front().probe.kind) {
        case MediaProbeKind::movie: return CatalogueKind::movie;
        case MediaProbeKind::episode: return CatalogueKind::episode;
        case MediaProbeKind::track: return CatalogueKind::track;
        }
    }
    if (provider == "movies") return CatalogueKind::movie;
    if (provider == "tv") return CatalogueKind::episode;
    if (provider == "music") return CatalogueKind::track;
    return {};
}

std::string suggested_query(const std::vector<MediaProbeCandidate>& probes, std::string_view path) {
    if (!probes.empty()) {
        const auto& probe = probes.front().probe;
        if (probe.kind == MediaProbeKind::episode && !probe.series.empty()) return probe.series;
        if (probe.kind == MediaProbeKind::track) {
            if (!probe.title.empty()) return probe.title;
            if (!probe.album.empty()) return probe.album;
        }
        if (!probe.title.empty()) return probe.title;
    }
    return filename_query(path);
}

std::string manual_id(std::string_view kind, std::string_view identity) {
    std::string seed = "manual|" + std::string(kind) + "|" + std::string(identity);
    const auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(seed.data()), seed.size());
    auto value = hex(sha256(bytes).bytes);
    value.resize(32);
    return "manual:" + std::string(kind) + ":" + value;
}

void preserve_existing(CatalogueManager& catalogue, CatalogueItem& item) {
    auto existing = catalogue.get(item.id);
    if (!existing) return;
    if (item.sort_title.empty()) item.sort_title = existing->sort_title;
    if (item.synopsis.empty()) item.synopsis = existing->synopsis;
    if (!item.year) item.year = existing->year;
    if (item.aliases.empty()) item.aliases = existing->aliases;
    if (item.artwork.empty()) item.artwork = existing->artwork;
    for (const auto& media_id : existing->media_ids)
        if (std::find(item.media_ids.begin(), item.media_ids.end(), media_id) == item.media_ids.end())
            item.media_ids.push_back(media_id);
    for (const auto& [key, value] : existing->external_ids)
        if (!item.external_ids.contains(key)) item.external_ids[key] = value;
}

CatalogueItem manual_base(CatalogueKind kind, std::string id, std::string title,
                          std::string synopsis = {}) {
    CatalogueItem item;
    item.id = std::move(id);
    item.kind = kind;
    item.title = std::move(title);
    item.sort_title = item.title;
    item.synopsis = std::move(synopsis);
    item.external_ids["macha_manual"] = "1";
    return item;
}

struct ManualItems {
    std::vector<CatalogueItem> items;
    std::string leaf_id;
};

ManualItems build_manual_items(CatalogueManager& catalogue, const Json& root,
                               std::string_view media_id) {
    const auto kind = required_string(root, "kind");
    const auto synopsis = string_value(root, "synopsis");
    ManualItems out;

    if (kind == "movie") {
        const auto title = required_string(root, "title");
        const auto year = optional_i32(root, "year");
        auto identity = title + "|" + (year ? std::to_string(*year) : "");
        auto item = manual_base(CatalogueKind::movie, manual_id("movie", identity), title, synopsis);
        item.year = year;
        item.media_ids.emplace_back(media_id);
        preserve_existing(catalogue, item);
        out.leaf_id = item.id;
        out.items.push_back(std::move(item));
        return out;
    }

    if (kind == "episode") {
        const auto series_title = required_string(root, "series");
        const auto series_year = optional_i32(root, "series_year");
        const auto season_number = optional_i32(root, "season_number");
        const auto episode_number = optional_i32(root, "episode_number");
        if (!season_number || !episode_number)
            throw std::runtime_error("season_number and episode_number are required");
        const auto episode_title = string_value(root, "title",
            "Episode " + std::to_string(*episode_number));

        const auto show_identity = series_title + "|" +
            (series_year ? std::to_string(*series_year) : "");
        auto show = manual_base(CatalogueKind::show, manual_id("show", show_identity), series_title);
        show.year = series_year;
        preserve_existing(catalogue, show);

        auto season = manual_base(CatalogueKind::season,
            manual_id("season", show.id + "|" + std::to_string(*season_number)),
            "Season " + std::to_string(*season_number));
        season.parent_id = show.id;
        season.season_number = season_number;
        preserve_existing(catalogue, season);

        auto episode = manual_base(CatalogueKind::episode,
            manual_id("episode", show.id + "|" + std::to_string(*season_number) + "|" +
                                 std::to_string(*episode_number)),
            episode_title, synopsis);
        episode.parent_id = season.id;
        episode.season_number = season_number;
        episode.episode_number = episode_number;
        episode.media_ids.emplace_back(media_id);
        preserve_existing(catalogue, episode);

        out.leaf_id = episode.id;
        out.items = {std::move(show), std::move(season), std::move(episode)};
        return out;
    }

    if (kind == "track") {
        const auto artist_title = required_string(root, "artist");
        const auto album_title = required_string(root, "album");
        const auto track_title = required_string(root, "title");
        const auto year = optional_i32(root, "year");
        const auto disc = optional_i32(root, "disc_number");
        const auto track_number = optional_i32(root, "track_number");

        auto artist = manual_base(CatalogueKind::artist,
            manual_id("artist", artist_title), artist_title);
        preserve_existing(catalogue, artist);

        const auto album_identity = artist.id + "|" + album_title + "|" +
            (year ? std::to_string(*year) : "");
        auto album = manual_base(CatalogueKind::album, manual_id("album", album_identity), album_title);
        album.parent_id = artist.id;
        album.year = year;
        preserve_existing(catalogue, album);

        const auto track_identity = album.id + "|" + (disc ? std::to_string(*disc) : "") + "|" +
            (track_number ? std::to_string(*track_number) : "") + "|" + track_title;
        auto track = manual_base(CatalogueKind::track, manual_id("track", track_identity), track_title, synopsis);
        track.parent_id = album.id;
        track.disc_number = disc;
        track.track_number = track_number;
        track.media_ids.emplace_back(media_id);
        preserve_existing(catalogue, track);

        out.leaf_id = track.id;
        out.items = {std::move(artist), std::move(album), std::move(track)};
        return out;
    }

    throw std::runtime_error("kind must be movie, episode or track");
}

std::optional<CatalogueHint> actionable_hint(CatalogueHintQueue& hints, std::string_view id) {
    auto hint = hints.get(id);
    if (!hint || hint->state != CatalogueHintState::no_match || hint->media_id.empty()) return {};
    return hint;
}

std::string route_id(std::string_view path, std::string_view prefix, std::string_view suffix = {}) {
    if (!path.starts_with(prefix)) return {};
    auto rest = path.substr(prefix.size());
    if (!suffix.empty()) {
        if (!rest.ends_with(suffix) || rest.size() <= suffix.size()) return {};
        rest.remove_suffix(suffix.size());
    }
    if (rest.empty() || rest.find('/') != std::string_view::npos) return {};
    return std::string(rest);
}

} // namespace

HttpResponse ManageApi::handle(const HttpRequest& request) {
    // Management writes are intentionally serialized. Combined with immutable
    // media-id verification below this prevents two stale UI sessions from both
    // resolving or deleting the same exception through this API.
    std::unique_lock mutation_lock(mutation_mutex_, std::defer_lock);
    if (request.method != "GET") mutation_lock.lock();
    try {
        if (request.method == "GET" && request.path == "/api/v1/manage/unmatched") {
            Json::Array items;
            for (const auto& hint : hints_.list()) {
                if (hint.state != CatalogueHintState::no_match || hint.media_id.empty()) continue;
                if (!current_hint_file(fs_, hint)) continue;
                items.push_back(hint_json(fs_, hint));
            }
            Json::Object out;
            out["count"] = static_cast<uint64_t>(items.size());
            out["items"] = std::move(items);
            return http_json(200, Json(std::move(out)).dump());
        }

        constexpr std::string_view unmatched_prefix = "/api/v1/manage/unmatched/";
        if (request.path.starts_with(unmatched_prefix)) {
            if (auto id = route_id(request.path, unmatched_prefix, "/matches"); !id.empty() &&
                request.method == "GET") {
                auto hint = actionable_hint(hints_, id);
                if (!hint) return http_error(404, "not_found", "unmatched file not found");
                auto current = current_hint_file(fs_, *hint);
                if (!current) return http_error(409, "stale_unmatched", "file changed or moved since matching failed");
                auto probes = scanner_.probe_unmatched(id);
                auto query = request.query.contains("q") ? request.query.at("q") : suggested_query(probes, hint->path);
                size_t limit = 12;
                if (auto it = request.query.find("limit"); it != request.query.end() && !it->second.empty()) {
                    auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), limit);
                    if (ec != std::errc{} || end != it->second.data() + it->second.size() || limit > 50)
                        return http_error(400, "bad_limit", "limit must be 0..50");
                }
                const auto wanted_kind = leaf_kind(probes, hint->provider);
                Json::Array matches;
                for (const auto& item : catalogue_.search(query, std::max<size_t>(limit * 4, 20))) {
                    if (wanted_kind && item.kind != *wanted_kind) continue;
                    matches.push_back(catalogue_item_json(item));
                    if (matches.size() >= limit) break;
                }
                Json::Object out;
                out["query"] = query;
                out["matches"] = std::move(matches);
                return http_json(200, Json(std::move(out)).dump());
            }

            if (auto id = route_id(request.path, unmatched_prefix, "/retry"); !id.empty() &&
                request.method == "POST") {
                auto hint = actionable_hint(hints_, id);
                if (!hint) return http_error(404, "not_found", "unmatched file not found");
                auto current = current_hint_file(fs_, *hint);
                if (!current) return http_error(409, "stale_unmatched", "file changed or moved since matching failed");
                hints_.submit(hint->path, "manual", current->second, CatalogueHintPriority::manual_rescan);
                return http_json(202, "{\"state\":\"queued\"}");
            }

            if (auto id = route_id(request.path, unmatched_prefix, "/match"); !id.empty() &&
                request.method == "POST") {
                auto hint = actionable_hint(hints_, id);
                if (!hint) return http_error(404, "not_found", "unmatched file not found");
                auto current = current_hint_file(fs_, *hint);
                if (!current) return http_error(409, "stale_unmatched", "file changed or moved since matching failed");
                const auto root = parse_body(request);
                const auto item_id = required_string(root, "catalogue_item_id");
                auto item = catalogue_.get(item_id);
                if (!item) return http_error(404, "catalogue_item_not_found", "catalogue item not found");
                if (item->kind != CatalogueKind::movie && item->kind != CatalogueKind::episode &&
                    item->kind != CatalogueKind::track)
                    return http_error(400, "not_playable_item", "files may be matched only to movies, episodes or tracks");
                if (std::find(item->media_ids.begin(), item->media_ids.end(), current->second) == item->media_ids.end())
                    item->media_ids.push_back(current->second);
                auto saved = catalogue_.upsert(*item, item->revision);
                hints_.mark_catalogued(hint->id, "manual", current->second, {saved.id},
                                       "manually matched to existing catalogue item");
                Json::Object out;
                out["item"] = catalogue_item_json(saved);
                return http_json(200, Json(std::move(out)).dump());
            }

            if (auto id = route_id(request.path, unmatched_prefix, "/manual"); !id.empty() &&
                request.method == "POST") {
                auto hint = actionable_hint(hints_, id);
                if (!hint) return http_error(404, "not_found", "unmatched file not found");
                auto current = current_hint_file(fs_, *hint);
                if (!current) return http_error(409, "stale_unmatched", "file changed or moved since matching failed");
                auto manual = build_manual_items(catalogue_, parse_body(request), current->second);
                auto saved = catalogue_.upsert_many(std::move(manual.items));
                hints_.mark_catalogued(hint->id, "manual", current->second, {manual.leaf_id},
                                       "manual catalogue metadata");
                Json::Array items;
                for (const auto& item : saved) items.push_back(catalogue_item_json(item));
                Json::Object out;
                out["leaf_item_id"] = manual.leaf_id;
                out["items"] = std::move(items);
                return http_json(201, Json(std::move(out)).dump());
            }

            if (auto id = route_id(request.path, unmatched_prefix); !id.empty()) {
                if (request.method == "GET") {
                    auto hint = actionable_hint(hints_, id);
                    if (!hint) return http_error(404, "not_found", "unmatched file not found");
                    auto current = current_hint_file(fs_, *hint);
                    if (!current) return http_error(409, "stale_unmatched", "file changed or moved since matching failed");
                    Json::Object out;
                    out["item"] = hint_json(fs_, *hint);
                    Json::Array probes;
                    for (const auto& probe : scanner_.probe_unmatched(id)) probes.push_back(probe_json(probe));
                    out["probes"] = std::move(probes);
                    return http_json(200, Json(std::move(out)).dump());
                }
                if (request.method == "DELETE") {
                    auto hint = actionable_hint(hints_, id);
                    if (!hint) return http_error(404, "not_found", "unmatched file not found");
                    if (!current_hint_file(fs_, *hint))
                        return http_error(409, "stale_unmatched", "file changed or moved since matching failed");
                    fs_.unlink(hint->path);
                    hints_.erase(hint->id);
                    return {204, "application/json; charset=utf-8", {}, {}};
                }
            }
        }

        if (request.method == "GET" && request.path == "/api/v1/manage/filesystem") {
            const auto path = normalize_path(request.query.contains("path") ? request.query.at("path") : "/");
            auto directory = fs_.getattr(path);
            if (directory.type != EntryType::directory)
                return http_error(400, "not_directory", "path is not a directory");
            std::map<std::string, std::vector<std::string>, std::less<>> bindings;
            try {
                bindings = media_bindings(catalogue_.snapshot());
            } catch (const std::exception& e) {
                // MachaDFS browsing is independent of catalogue availability.
                // Binding annotations are a convenience only.
                Log::debug("manage MachaDFS browse without catalogue bindings: " +
                           std::string(e.what()));
            }
            Json::Array entries;
            for (const auto& [name, entry] : fs_.readdir(path))
                entries.push_back(fs_entry_json(entry_path(path, name), name, entry, bindings));
            std::sort(entries.begin(), entries.end(), [](const Json& a, const Json& b) {
                const auto ta = a.find("type")->asString();
                const auto tb = b.find("type")->asString();
                if (ta != tb) return ta == "directory";
                return a.find("name")->asString() < b.find("name")->asString();
            });
            Json::Object out;
            out["path"] = path;
            out["parent"] = path == "/" ? Json(nullptr) : Json(parent_path(path));
            out["entries"] = std::move(entries);
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "POST" && request.path == "/api/v1/manage/filesystem/mkdir") {
            const auto root = parse_body(request);
            const auto path = normalize_path(required_string(root, "path"));
            fs_.mkdir(path, 0755, getuid(), getgid());
            return http_json(201, "{\"path\":" + Json(path).dump() + "}");
        }

        if (request.method == "POST" && request.path == "/api/v1/manage/filesystem/rename") {
            const auto root = parse_body(request);
            const auto source = normalize_path(required_string(root, "path"));
            const auto destination = normalize_path(required_string(root, "destination"));
            const auto no_replace = bool_value(root, "no_replace", true);
            fs_.rename(source, destination, no_replace);
            hints_.rename_prefix(source, destination);
            Json::Object out;
            out["path"] = destination;
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "DELETE" && request.path == "/api/v1/manage/filesystem") {
            auto it = request.query.find("path");
            if (it == request.query.end() || it->second.empty())
                return http_error(400, "missing_path", "path is required");
            const auto path = normalize_path(it->second);
            if (path == "/") return http_error(400, "root_delete", "MachaDFS root cannot be deleted");
            const auto entry = fs_.getattr(path);
            if (entry.type == EntryType::directory) fs_.rmdir(path);
            else fs_.unlink(path);
            hints_.erase_prefix(path);
            return {204, "application/json; charset=utf-8", {}, {}};
        }

        return http_error(404, "not_found", "management route not found");
    } catch (const FsError& e) {
        int status = 400;
        if (e.code() == ENOENT) status = 404;
        else if (e.code() == EEXIST || e.code() == ENOTEMPTY) status = 409;
        return http_error(status, "filesystem_error", e.what());
    } catch (const CatalogueConflict& e) {
        return http_error(409, "catalogue_conflict", e.what());
    } catch (const CatalogueUnavailable& e) {
        return http_error(503, "catalogue_unavailable", e.what());
    } catch (const JsonError& e) {
        return http_error(400, "bad_json", e.what());
    } catch (const std::runtime_error& e) {
        return http_error(400, "bad_request", e.what());
    } catch (const std::exception& e) {
        Log::warn("manage API failed: " + std::string(e.what()));
        return http_error(500, "internal_error", e.what());
    }
}

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/manage_api.hpp"
#include "api/paging.hpp"

#include "crypto.hpp"
#include "json.hpp"
#include "log.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <limits>
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

// A current hint's file as the listing needs it: size and mtime. The media
// index answers for a file kept under the hint's own path; any other spelling
// takes the whole lookup.
std::optional<FsEntry> current_hint_stat(FileSystem& fs, const CatalogueHint& hint) {
    if (!hint.media_id.empty())
        if (auto entry = fs.media_stat(hint.media_id, hint.path); entry && entry->size != 0)
            return entry;
    if (auto current = current_hint_file(fs, hint))
        return std::move(current->first);
    return {};
}

Json hint_json(const CatalogueHint& hint, const std::optional<FsEntry>& current) {
    Json::Object out;
    out["id"] = hint.id;
    out["path"] = hint.path;
    out["provider"] = hint.provider.empty() ? Json(nullptr) : Json(hint.provider);
    out["media_id"] = hint.media_id.empty() ? Json(nullptr) : Json(hint.media_id);
    out["result"] = hint.result;
    out["attempts"] = static_cast<uint64_t>(hint.attempts);
    out["updated_unix_ms"] = hint.updated_unix_ms;
    if (current) {
        out["size"] = current->size;
        out["mtime_ns"] = current->mtime_ns;
        out["current"] = true;
    } else {
        out["size"] = static_cast<uint64_t>(0);
        out["mtime_ns"] = static_cast<int64_t>(0);
        out["current"] = false;
    }
    return Json(std::move(out));
}

Json hint_json(FileSystem& fs, const CatalogueHint& hint) {
    return hint_json(hint, current_hint_stat(fs, hint));
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

// A parent named by id that cannot take the manual item: 404 when it does not
// exist, 400 when it is the wrong kind. `fields` are stated in the error.
struct ManualParentError : std::runtime_error {
    int status;
    std::string code;
    std::vector<std::pair<std::string, std::string>> fields;
    ManualParentError(int http_status, std::string error_code, const std::string& message,
                      std::vector<std::pair<std::string, std::string>> extra)
        : std::runtime_error(message), status(http_status), code(std::move(error_code)),
          fields(std::move(extra)) {}
};

CatalogueItem require_parent(CatalogueManager& catalogue, const std::string& id,
                             CatalogueKind kind) {
    auto parent = catalogue.get(id);
    if (!parent)
        throw ManualParentError(404, "parent_not_found", "parent item not found",
                                {{"parent_id", id}});
    if (parent->kind != kind)
        throw ManualParentError(400, "bad_parent_kind", "parent is the wrong kind",
                                {{"parent_id", id},
                                 {"expected_kind", std::string(catalogue_kind_name(kind))},
                                 {"parent_kind", std::string(catalogue_kind_name(parent->kind))}});
    return *parent;
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
        auto season_number = optional_i32(root, "season_number");
        const auto episode_number = optional_i32(root, "episode_number");
        const auto season_id = string_value(root, "season_id");
        const auto series_id = string_value(root, "series_id");
        std::optional<CatalogueItem> existing_season;
        if (!season_id.empty()) {
            existing_season = require_parent(catalogue, season_id, CatalogueKind::season);
            if (existing_season->season_number) season_number = existing_season->season_number;
        }
        if (!season_number || !episode_number)
            throw std::runtime_error("season_number and episode_number are required");
        const auto episode_title = string_value(root, "title",
            "Episode " + std::to_string(*episode_number));

        // An existing season or show named by id: the episode joins the
        // scanner's (or anyone's) hierarchy instead of a duplicate by name.
        if (existing_season || !series_id.empty()) {
            std::vector<CatalogueItem> written;
            CatalogueItem season;
            if (existing_season) {
                season = *existing_season;
            } else {
                const auto show = require_parent(catalogue, series_id, CatalogueKind::show);
                std::optional<CatalogueItem> found;
                for (const auto& candidate : catalogue.list(CatalogueKind::season, show.id))
                    if (candidate.season_number == season_number) { found = candidate; break; }
                if (found) {
                    season = *found;
                } else {
                    season = manual_base(CatalogueKind::season,
                        manual_id("season", show.id + "|" + std::to_string(*season_number)),
                        "Season " + std::to_string(*season_number));
                    season.parent_id = show.id;
                    season.season_number = season_number;
                    preserve_existing(catalogue, season);
                    written.push_back(season);
                }
            }
            auto episode = manual_base(CatalogueKind::episode,
                manual_id("episode", season.id + "|" + std::to_string(*episode_number)),
                episode_title, synopsis);
            episode.parent_id = season.id;
            episode.season_number = season_number;
            episode.episode_number = episode_number;
            episode.media_ids.emplace_back(media_id);
            preserve_existing(catalogue, episode);
            out.leaf_id = episode.id;
            written.push_back(std::move(episode));
            out.items = std::move(written);
            return out;
        }

        const auto series_title = required_string(root, "series");
        const auto series_year = optional_i32(root, "series_year");

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
        const auto track_title = required_string(root, "title");
        const auto year = optional_i32(root, "year");
        const auto disc = optional_i32(root, "disc_number");
        const auto track_number = optional_i32(root, "track_number");
        const auto album_id = string_value(root, "album_id");
        const auto artist_id = string_value(root, "artist_id");

        // An existing album, or an existing artist with the album by title.
        if (!album_id.empty() || !artist_id.empty()) {
            std::vector<CatalogueItem> written;
            CatalogueItem album;
            if (!album_id.empty()) {
                album = require_parent(catalogue, album_id, CatalogueKind::album);
            } else {
                const auto artist = require_parent(catalogue, artist_id, CatalogueKind::artist);
                const auto album_title = required_string(root, "album");
                std::optional<CatalogueItem> found;
                for (const auto& candidate : catalogue.list(CatalogueKind::album, artist.id))
                    if (candidate.title == album_title && (!year || candidate.year == year)) {
                        found = candidate;
                        break;
                    }
                if (found) {
                    album = *found;
                } else {
                    album = manual_base(CatalogueKind::album,
                        manual_id("album", artist.id + "|" + album_title + "|" +
                                               (year ? std::to_string(*year) : "")),
                        album_title);
                    album.parent_id = artist.id;
                    album.year = year;
                    preserve_existing(catalogue, album);
                    written.push_back(album);
                }
            }
            const auto track_identity = album.id + "|" + (disc ? std::to_string(*disc) : "") + "|" +
                (track_number ? std::to_string(*track_number) : "") + "|" + track_title;
            auto track = manual_base(CatalogueKind::track, manual_id("track", track_identity),
                                     track_title, synopsis);
            track.parent_id = album.id;
            track.disc_number = disc;
            track.track_number = track_number;
            track.media_ids.emplace_back(media_id);
            preserve_existing(catalogue, track);
            out.leaf_id = track.id;
            written.push_back(std::move(track));
            out.items = std::move(written);
            return out;
        }

        const auto artist_title = required_string(root, "artist");
        const auto album_title = required_string(root, "album");

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

std::optional<NodeId> parse_node_id(std::string_view text) {
    auto raw = unhex(std::string(text));
    if (!raw || raw->size() != 16) return {};
    NodeId id;
    std::copy(raw->begin(), raw->end(), id.bytes.begin());
    return id;
}

Json identity_reset_json(const IdentityAssociationReset& reset) {
    Json::Object out;
    out["scope"] = identity_reset_key(reset.host, reset.port);
    out["host"] = reset.host;
    out["port"] = reset.port ? Json(static_cast<uint64_t>(reset.port)) : Json(nullptr);
    out["stale_node_id"] = reset.stale_node_id == NodeId{}
                               ? Json(nullptr)
                               : Json(to_string(reset.stale_node_id));
    out["epoch"] = reset.epoch;
    out["reset_at_unix_ms"] = reset.reset_unix_ms;
    out["reset_by_node_id"] = to_string(reset.reset_by);
    out["reason"] = reset.reason.empty() ? Json(nullptr) : Json(reset.reason);
    return Json(std::move(out));
}

} // namespace

ManageApi::ManageApi(NodeRuntime& node, MetadataView& metadata, FileSystem& fs,
                     CatalogueManager& catalogue, CatalogueHintQueue& hints,
                     CatalogueScanner& scanner)
    : node_(node), metadata_(metadata), fs_(fs), catalogue_(catalogue), hints_(hints),
      scanner_(scanner),
      identity_audit_worker_([this](std::stop_token stop) {
          identity_reset_audit_loop(stop);
      }) {}

ManageApi::~ManageApi() {
    stop();
}

void ManageApi::request_stop() {
    identity_audit_worker_.request_stop();
    identity_audit_cv_.notify_all();
}

void ManageApi::stop() {
    request_stop();
    if (identity_audit_worker_.joinable())
        identity_audit_worker_.join();
}

void ManageApi::queue_identity_reset_audit(IdentityAssociationReset reset) {
    const auto key = identity_reset_key(reset.host, reset.port);
    {
        Lock lock(identity_audit_mutex_);
        auto found = identity_audit_pending_.find(key);
        if (found == identity_audit_pending_.end() || found->second.epoch < reset.epoch)
            identity_audit_pending_[key] = std::move(reset);
    }
    identity_audit_cv_.notify_one();
}

void ManageApi::identity_reset_audit_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        IdentityAssociationReset reset;
        {
            Lock lock(identity_audit_mutex_);
            identity_audit_cv_.wait(lock.native(), stop, [&]() MACHA_REQUIRES(identity_audit_mutex_) {
                return !identity_audit_pending_.empty();
            });
            if (stop.stop_requested())
                break;
            auto found = identity_audit_pending_.begin();
            reset = std::move(found->second);
            identity_audit_pending_.erase(found);
        }

        const auto key = identity_reset_key(reset.host, reset.port);
        try {
            // The locally durable tombstone is the commit; peer propagation and the
            // cluster-metadata audit converge it outside the request.
            node_.propagate_identity_reset(reset);
            const auto committed = metadata_.mutate_delta(
                [&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
                    if (auto found = snapshot.identity_resets.find(key);
                        found != snapshot.identity_resets.end() &&
                        found->second.epoch >= reset.epoch) {
                        if (found->second.epoch == std::numeric_limits<uint64_t>::max())
                            throw std::runtime_error("identity association reset epoch exhausted");
                        reset.epoch = found->second.epoch + 1;
                    }
                    snapshot.identity_resets[key] = reset;
                    delta.upsert_identity_resets[key] = reset;
                });
            node_.propagate_identity_reset(reset);
            Log::info("management identity reset audit completed scope=" + key +
                      " epoch=" + std::to_string(reset.epoch) +
                      " metadata_generation=" + std::to_string(committed.generation));
        } catch (const std::exception& error) {
            // The reset stays durable in Membership and travels in later identity-reset
            // exchanges; a later explicit reset can retry the audit.
            Log::warn("management identity reset audit deferred scope=" + key +
                      " epoch=" + std::to_string(reset.epoch) + " error=" + error.what());
        }
    }
}

namespace {

constexpr std::string_view items_prefix = "/api/v1/catalogue/items/";
constexpr std::string_view media_infix = "/media/";
constexpr std::string_view files_root = "/api/v1/files";

// "{id}/media/{media_id}" of an unmatch path, or nothing.
std::optional<std::pair<std::string, std::string>> unmatch_target(std::string_view path) {
    if (!path.starts_with(items_prefix))
        return {};
    const auto rest = path.substr(items_prefix.size());
    const auto infix = rest.rfind(media_infix);
    if (infix == std::string_view::npos || infix == 0 ||
        infix + media_infix.size() == rest.size())
        return {};
    const auto media = rest.substr(infix + media_infix.size());
    if (media.find('/') != std::string_view::npos)
        return {};
    return std::pair{std::string(rest.substr(0, infix)), std::string(media)};
}

constexpr std::string_view metadata_suffix = "/metadata";

// The item id of a Clear Metadata path, or nothing.
std::optional<std::string> clear_metadata_target(std::string_view path) {
    if (!path.starts_with(items_prefix) || !path.ends_with(metadata_suffix))
        return {};
    const auto id = path.substr(items_prefix.size(),
                                path.size() - items_prefix.size() - metadata_suffix.size());
    if (id.empty() || id.find('/') != std::string_view::npos)
        return {};
    return std::string(id);
}

std::optional<uint64_t> if_match_revision(const HttpRequest& request) {
    auto it = request.headers.find("if-match");
    if (it == request.headers.end())
        return {};
    auto value = it->second;
    value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
    if (value.starts_with("rev-")) value.erase(0, 4);
    uint64_t revision{};
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), revision);
    if (ec != std::errc{} || end != value.data() + value.size())
        throw std::runtime_error("invalid If-Match revision");
    return revision;
}

Json string_array(const std::vector<std::string>& values) {
    Json::Array out;
    for (const auto& value : values) out.emplace_back(value);
    return Json(std::move(out));
}

} // namespace

bool ManageApi::title_file_route(const HttpRequest& request) {
    if (request.method != "DELETE")
        return false;
    if (request.path == files_root || request.path.starts_with(std::string(files_root) + "/"))
        return true;
    return unmatch_target(request.path).has_value() ||
           clear_metadata_target(request.path).has_value();
}

HttpResponse ManageApi::title_files(const HttpRequest& request) {
    // A file goes to the unmatched list as it is: no provider is asked.
    const auto unmatched = [&](const std::string& media_id) {
        for (const auto& path : fs_.media_paths(media_id))
            hints_.put_unmatched(path, media_id);
    };

    // Clear Metadata: the item and everything beneath it go, and their files
    // go to the unmatched list to be identified by hand.
    if (const auto item_id = clear_metadata_target(request.path)) {
        const auto revision = if_match_revision(request);
        if (catalogue_.definitely_absent(*item_id))
            return http_error(404, "not_found", "catalogue item not found");
        const auto cleared = catalogue_.clear_metadata_with_media(*item_id, revision);
        if (!cleared.removed_items)
            return http_error(404, "not_found", "catalogue item not found");
        for (const auto& media_id : cleared.media_ids)
            unmatched(media_id);
        return {204, "application/json; charset=utf-8", {}, {}};
    }

    if (const auto target = unmatch_target(request.path)) {
        const auto& [item_id, media_id] = *target;
        const auto result =
            catalogue_.unbind_media(std::string_view(item_id), media_id, if_match_revision(request));
        if (!result.found)
            return http_error(404, "not_found", "catalogue item not found");
        if (!result.bound)
            return http_error(404, "media_not_bound", "the item does not hold that media id");
        unmatched(media_id);
        Json::Object out;
        out["status"] = "unmatched";
        if (result.item)
            out["item"] = catalogue_item_json(*result.item);
        out["removed_item_ids"] = string_array(result.removed_ids);
        return http_json(200, Json(std::move(out)).dump());
    }

    // Deleted content is unbound only once no path holds it any longer.
    const auto unbind_if_gone = [&](const std::string& media_id) {
        if (media_id.empty() || !fs_.media_paths(media_id).empty())
            return std::vector<std::string>{};
        return catalogue_.unbind_media(std::nullopt, media_id).removed_ids;
    };

    if (request.path == files_root || request.path == std::string(files_root) + "/") {
        const auto hash = request.query.find("hash");
        if (hash == request.query.end() || hash->second.empty())
            return http_error(400, "missing_hash", "hash is required to delete by content");
        const auto paths = fs_.media_paths(hash->second);
        if (paths.empty())
            return http_error(404, "not_found", "no file holds that content");
        for (const auto& path : paths) {
            fs_.unlink(path);
            hints_.erase_prefix(path);
        }
        Json::Object out;
        out["status"] = "deleted";
        out["paths"] = string_array(paths);
        out["removed_item_ids"] = string_array(unbind_if_gone(hash->second));
        return http_json(200, Json(std::move(out)).dump());
    }

    const auto path = normalize_path(request.path.substr(files_root.size()));
    if (path == "/")
        return http_error(409, "not_a_file", "the root is a directory");
    FsEntry entry;
    try {
        entry = fs_.getattr(path);
    } catch (const FsError& error) {
        if (error.code() == ENOENT || error.code() == ENOTDIR)
            return http_error(404, "not_found", "no such file");
        throw;
    }
    if (entry.type != EntryType::file)
        return http_error(409, "not_a_file", "a directory is removed through the filesystem");
    const auto media_id = entry.size ? file_media_id(entry) : std::string{};
    fs_.unlink(path);
    hints_.erase_prefix(path);
    Json::Object out;
    out["status"] = "deleted";
    out["path"] = path;
    out["removed_item_ids"] = string_array(unbind_if_gone(media_id));
    return http_json(200, Json(std::move(out)).dump());
}

HttpResponse ManageApi::handle(const HttpRequest& request) {
    auto response = dispatch(request);
    // A write that was refused, and any request that failed here or
    // upstream, says why in the journal: the answer's code and message.
    if (response.status >= 500 || (response.status >= 400 && request.method != "GET")) {
        const auto text = "manage API " + request.method + " " + request.path + " status=" +
                          std::to_string(response.status) + " " +
                          std::string(response.body.begin(),
                                      response.body.begin() +
                                          static_cast<std::ptrdiff_t>(
                                              std::min<size_t>(response.body.size(), 300)));
        if (response.status >= 500)
            Log::warn(text);
        else
            Log::info(text);
    }
    return response;
}

HttpResponse ManageApi::dispatch(const HttpRequest& request) {
    // Management writes run concurrently and share commits. Each verifies the
    // media id it was given, so two stale UI sessions cannot both resolve or
    // delete the same exception.
    try {
        if (title_file_route(request))
            return title_files(request);
        if (request.method == "GET" && request.path == "/api/v1/manage") {
            Json::Object resources;
            resources["unmatched"] = "/api/v1/manage/unmatched";
            resources["filesystem"] = "/api/v1/manage/filesystem";
            Json::Object actions;
            actions["identity_association_reset"] =
                "/api/v1/manage/identity-associations/reset";
            actions["node_identity_association_reset"] =
                "/api/v1/manage/nodes/{node_id}/identity-association/reset";
            Json::Object out;
            out["api"] = "manage";
            out["version"] = static_cast<uint64_t>(1);
            out["privileged"] = false;
            out["resources"] = std::move(resources);
            out["actions"] = std::move(actions);
            return http_json(200, Json(std::move(out)).dump());
        }

        auto commit_identity_reset = [&](std::string host, uint16_t port, NodeId stale_id,
                                         std::string reason) -> HttpResponse {
            if (host.empty())
                return http_error(400, "host_required", "host/IP is required");

            IdentityAssociationReset reset;
            reset.host = std::move(host);
            reset.port = port; // 0 deliberately means every port on this host.
            reset.stale_node_id = stale_id; // zero deliberately means unknown/any stale identity.
            reset.reset_by = node_.node_id();
            reset.reset_unix_ms = unix_ms();
            reset.reason = std::move(reason);
            const auto key = identity_reset_key(reset.host, reset.port);

            // A recovery primitive: it must not depend on metadata whose convergence the
            // stale identity may fence. Allocate from every local tombstone, apply and
            // propagate, then publish the durable metadata audit.
            reset.epoch = 1;
            auto advance_epoch = [&](const IdentityAssociationReset& existing) {
                if (existing.epoch < reset.epoch)
                    return;
                if (existing.epoch == std::numeric_limits<uint64_t>::max())
                    throw std::runtime_error("identity association reset epoch exhausted");
                reset.epoch = existing.epoch + 1;
            };
            for (const auto& existing : node_.identity_resets())
                if (identity_reset_key(existing.host, existing.port) == key)
                    advance_epoch(existing);

            // The request commits only the local recovery record; slow propagation and
            // auditing belong to the asynchronous audit worker.
            node_.apply_identity_reset(reset);
            queue_identity_reset_audit(reset);

            Log::info("management identity reset accepted scope=" + key +
                      " stale_node_id=" +
                      (stale_id == NodeId{} ? std::string("<any>") : to_string(stale_id)) +
                      " epoch=" + std::to_string(reset.epoch) +
                      " audit=queued" +
                      (reset.reason.empty() ? std::string{} : " reason=" + reset.reason));

            Json::Object out;
            out["reset"] = identity_reset_json(reset);
            out["audit_state"] = "queued";
            out["metadata_persisted"] = false;
            out["metadata_generation"] = Json(nullptr);
            out["persistence_error"] = Json(nullptr);
            return http_json(202, Json(std::move(out)).dump());
        };

        // Cluster-wide association reset; no NodeId required:
        //   host + port + node_id => one known endpoint->NodeId association
        //   host + port           => whatever stale identity occupied that endpoint
        //   host                  => all stale endpoint associations on that host
        // Wildcard forms suppress pre-reset gossip but let a fresh, directly
        // authenticated peer establish a replacement association.
        if (request.method == "POST" &&
            request.path == "/api/v1/manage/identity-associations/reset") {
            const auto body = parse_body(request);
            auto host = string_value(body, "host");
            uint16_t port = 0;
            if (const auto* value = body.find("port"); value && !value->isNull()) {
                const auto raw = value->asUInt64();
                if (!raw || raw > 65535)
                    return http_error(400, "bad_port", "port must be 1..65535 when supplied");
                port = static_cast<uint16_t>(raw);
            }
            NodeId stale_id{};
            if (const auto id_text = string_value(body, "node_id"); !id_text.empty()) {
                const auto parsed = parse_node_id(id_text);
                if (!parsed)
                    return http_error(400, "bad_node_id",
                                      "node_id must be a 32-character hexadecimal id");
                stale_id = *parsed;
            }
            return commit_identity_reset(std::move(host), port, stale_id,
                                         string_value(body, "reason"));
        }

        // Node-scoped route, used by Status node detail.
        constexpr std::string_view node_manage_prefix = "/api/v1/manage/nodes/";
        if (request.method == "POST" && request.path.starts_with(node_manage_prefix)) {
            const auto id_text = route_id(request.path, node_manage_prefix,
                                          "/identity-association/reset");
            if (!id_text.empty()) {
                const auto stale_id = parse_node_id(id_text);
                if (!stale_id)
                    return http_error(400, "bad_node_id",
                                      "node id must be a 32-character hexadecimal id");

                const auto body = parse_body(request);
                std::string host = string_value(body, "host");
                uint16_t port = 0;
                if (const auto* value = body.find("port"); value && !value->isNull()) {
                    const auto raw = value->asUInt64();
                    if (!raw || raw > 65535)
                        return http_error(400, "bad_port", "port must be 1..65535 when supplied");
                    port = static_cast<uint16_t>(raw);
                }

                // Without an endpoint, use the node's latest from live membership, then
                // durable status. A host alone keeps port=0: a host-wide reset for this NodeId.
                if (host.empty()) {
                    for (const auto& member : node_.membership().all()) {
                        if (member.id == *stale_id) {
                            host = member.host;
                            port = member.port;
                            break;
                        }
                    }
                    if (host.empty()) {
                        auto view = metadata_.converged(WorkContext(
                            FrameType::control, {}, nullptr, "POST /api/v1/manage/nodes/{id}"));
                        if (auto found = view.snapshot->node_status.find(*stale_id);
                            found != view.snapshot->node_status.end()) {
                            host = found->second.host;
                            port = found->second.port;
                        }
                    }
                }
                if (host.empty())
                    return http_error(404, "node_endpoint_unknown",
                                      "no known endpoint for this node; supply host/IP");

                return commit_identity_reset(std::move(host), port, *stale_id,
                                             string_value(body, "reason"));
            }
        }

        if (request.method == "GET" && request.path == "/api/v1/manage/unmatched") {
            PageQuery page;
            if (auto bad = read_page_query(request, page)) return *bad;
            Json::Array items;
            for (const auto& hint : hints_.list()) {
                if (hint.state != CatalogueHintState::no_match || hint.media_id.empty()) continue;
                const auto current = current_hint_stat(fs_, hint);
                if (!current) continue;
                items.push_back(hint_json(hint, current));
            }
            // A file bound to more than one playable item, except the one
            // legitimate case: a multi-episode file, bound to several episodes
            // of one season.
            std::map<std::string, std::vector<const CatalogueItem*>> bound;
            const auto snapshot = catalogue_.snapshot_view(
                WorkContext(FrameType::control, {}, nullptr, "GET /api/v1/manage/unmatched"));
            for (const auto& [_, item] : snapshot->items) {
                if (item.kind != CatalogueKind::movie && item.kind != CatalogueKind::episode &&
                    item.kind != CatalogueKind::track)
                    continue;
                for (const auto& media : item.media_ids) bound[media].push_back(&item);
            }
            Json::Array conflicts;
            for (const auto& [media, holders] : bound) {
                if (holders.size() < 2) continue;
                const bool one_season = std::all_of(holders.begin(), holders.end(), [&](const auto* item) {
                    return item->kind == CatalogueKind::episode && item->parent_id &&
                           item->parent_id == holders.front()->parent_id;
                });
                if (one_season) continue;
                Json::Array ids;
                for (const auto* item : holders) ids.emplace_back(item->id);
                conflicts.push_back(Json(Json::Object{{"media_id", media}, {"item_ids", Json(std::move(ids))}}));
            }
            Json::Object out;
            out["count"] = static_cast<uint64_t>(items.size());
            page_json(items, "id", page, out);
            out["items"] = std::move(items);
            out["conflicts"] = std::move(conflicts);
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "GET" && request.path == "/api/v1/manage/providers/search") {
            ProviderSearchQuery query;
            if (auto it = request.query.find("q"); it != request.query.end()) query.text = it->second;
            if (query.text.empty()) return http_error(400, "bad_query", "q is required");
            if (auto it = request.query.find("kind"); it != request.query.end()) query.kind = it->second;
            if (auto it = request.query.find("artist"); it != request.query.end()) query.artist = it->second;
            if (auto it = request.query.find("year"); it != request.query.end() && !it->second.empty()) {
                int32_t year = 0;
                auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), year);
                if (ec != std::errc{} || end != it->second.data() + it->second.size())
                    return http_error(400, "bad_year", "year must be a number");
                query.year = year;
            }
            if (auto it = request.query.find("limit"); it != request.query.end() && !it->second.empty()) {
                auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), query.limit);
                if (ec != std::errc{} || end != it->second.data() + it->second.size() ||
                    query.limit == 0 || query.limit > 50)
                    return http_error(400, "bad_limit", "limit must be 1..50");
            }
            Json::Array results;
            for (const auto& result : scanner_.search_providers(query)) {
                Json::Object item;
                item["ref"] = result.ref;
                item["provider"] = result.provider;
                item["kind"] = result.kind;
                item["title"] = result.title;
                item["year"] = result.year ? Json(static_cast<int64_t>(*result.year)) : Json(nullptr);
                item["overview"] = result.overview;
                if (result.kind == "album") item["artist"] = result.artist;
                if (!result.catalogue_id.empty()) item["catalogue_item_id"] = result.catalogue_id;
                results.push_back(Json(std::move(item)));
            }
            Json::Object out;
            out["status"] = "ok";
            out["results"] = std::move(results);
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "GET" && request.path == "/api/v1/manage/providers/artwork") {
            auto query_i32 = [&](std::string_view name) -> std::optional<int32_t> {
                auto it = request.query.find(name);
                if (it == request.query.end() || it->second.empty()) return {};
                int32_t value = 0;
                auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
                if (ec != std::errc{} || end != it->second.data() + it->second.size())
                    throw ProviderRequestError(400, "bad_number", std::string(name) + " must be a number");
                return value;
            };
            const auto ref = request.query.contains("ref") ? request.query.at("ref") : std::string{};
            const auto role = request.query.contains("role") ? request.query.at("role") : std::string{};
            ProviderRefNumbers numbers;
            numbers.season = query_i32("season_number");
            numbers.episode = query_i32("episode_number");
            Json::Array options;
            for (const auto& option : scanner_.artwork_options(ref, role, numbers)) {
                Json::Object item;
                item["option_id"] = option.option_id;
                item["role"] = option.role;
                item["width"] = option.width ? Json(static_cast<int64_t>(*option.width)) : Json(nullptr);
                item["height"] = option.height ? Json(static_cast<int64_t>(*option.height)) : Json(nullptr);
                item["language"] = option.language.empty() ? Json(nullptr) : Json(option.language);
                item["preview_url"] = option.preview_url;
                options.push_back(Json(std::move(item)));
            }
            Json::Object out;
            out["status"] = "ok";
            out["options"] = std::move(options);
            return http_json(200, Json(std::move(out)).dump());
        }

        if (auto id = route_id(request.path, "/api/v1/manage/providers/musicbrainz/releases/",
                               "/tracks");
            !id.empty() && request.method == "GET") {
            const auto number = [](const auto& value) {
                return value ? Json(static_cast<int64_t>(*value)) : Json(nullptr);
            };
            Json::Array tracks;
            for (const auto& track : scanner_.release_tracks("musicbrainz", id)) {
                Json::Object item;
                item["disc_number"] = number(track.disc_number);
                item["track_number"] = number(track.track_number);
                item["title"] = track.title;
                item["length_ms"] = number(track.length_ms);
                item["recording_id"] =
                    track.recording_id.empty() ? Json(nullptr) : Json(track.recording_id);
                tracks.push_back(Json(std::move(item)));
            }
            Json::Object out;
            out["status"] = "ok";
            out["tracks"] = std::move(tracks);
            return http_json(200, Json(std::move(out)).dump());
        }

        if (request.method == "POST" && request.path == "/api/v1/manage/providers/artwork/choose") {
            const auto body = parse_body(request);
            std::optional<std::string> ref;
            if (auto value = string_value(body, "ref"); !value.empty()) ref = std::move(value);
            ProviderRefNumbers numbers;
            numbers.season = optional_i32(body, "season_number");
            numbers.episode = optional_i32(body, "episode_number");
            bool lock = true;
            if (const auto* value = body.find("lock")) lock = value->asBool();
            auto item = scanner_.choose_artwork(required_string(body, "item_id"),
                                                required_string(body, "role"),
                                                required_string(body, "option_id"), std::move(ref),
                                                numbers, lock);
            Json::Object out;
            out["status"] = "chosen";
            out["item"] = catalogue_item_json(item);
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
                if (const auto ref = string_value(root, "ref"); !ref.empty()) {
                    ProviderRefNumbers numbers;
                    numbers.season = optional_i32(root, "season_number");
                    numbers.episode = optional_i32(root, "episode_number");
                    numbers.disc = optional_i32(root, "disc_number");
                    numbers.track = optional_i32(root, "track_number");
                    const auto matched = scanner_.match_unmatched_ref(id, ref, numbers);
                    Json::Array items;
                    for (const auto& item_id : matched.item_ids)
                        if (auto item = catalogue_.get(item_id)) items.push_back(catalogue_item_json(*item));
                    Json::Object out;
                    out["status"] = "matched";
                    out["leaf_item_id"] = matched.leaf_id;
                    out["items"] = std::move(items);
                    return http_json(200, Json(std::move(out)).dump());
                }
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
                                       "manual_existing_item");
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
                const auto body = parse_body(request);
                auto manual = build_manual_items(catalogue_, body, current->second);
                // Hand-entered metadata is locked against the scanner unless
                // the request says otherwise.
                bool lock = true;
                if (const auto* value = body.find("lock")) lock = value->asBool();
                for (auto& item : manual.items) {
                    if (lock)
                        item.external_ids["macha_metadata_locked"] = "1";
                    else
                        item.external_ids.erase("macha_metadata_locked");
                }
                auto saved = catalogue_.upsert_many(std::move(manual.items));
                hints_.mark_catalogued(hint->id, "manual", current->second, {manual.leaf_id},
                                       "manual_metadata");
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
            PageQuery page;
            if (auto bad = read_page_query(request, page)) return *bad;
            const auto path = normalize_path(request.query.contains("path") ? request.query.at("path") : "/");
            auto directory = fs_.getattr(path);
            if (directory.type != EntryType::directory)
                return http_error(400, "not_directory", "path is not a directory");
            std::map<std::string, std::vector<std::string>, std::less<>> bindings;
            try {
                (void)catalogue_.snapshot_view(
                    WorkContext(FrameType::control, {}, nullptr, "GET /api/v1/manage/filesystem"));
                bindings = catalogue_.indexes()->media_bindings;
            } catch (const std::exception& e) {
                // MachaDFS browsing does not depend on the catalogue; binding annotations are
                // a convenience.
                Log::debug("manage MachaDFS browse without catalogue bindings: " +
                           std::string(e.what()));
            }
            Json::Array entries;
            for (const auto& [name, entry] : fs_.readdir(path))
                entries.push_back(fs_entry_json(entry_path(path, name), name, entry, bindings));
            Json::Object out;
            out["path"] = path;
            out["parent"] = path == "/" ? Json(nullptr) : Json(parent_path(path));
            page_json(entries, "name", page, out);
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
    } catch (const ProviderRequestError& e) {
        auto response = http_error(e.status, e.code, e.what());
        if (e.retry_after) {
            // How long until the provider may answer, in the body and as
            // Retry-After.
            Json::Object root;
            root["status"] = e.code;
            root["error"] = Json::Object{{"code", e.code},
                                         {"message", std::string(e.what())},
                                         {"retry_after_ms", static_cast<uint64_t>(e.retry_after->count())}};
            response = http_json(e.status, Json(std::move(root)).dump());
            response.headers["Retry-After"] =
                std::to_string((e.retry_after->count() + 999) / 1000);
        }
        return response;
    } catch (const ManualParentError& e) {
        Json::Object error{{"code", e.code}, {"message", std::string(e.what())}};
        for (const auto& [name, value] : e.fields) error[name] = value;
        Json::Object root;
        root["status"] = e.code;
        root["error"] = std::move(error);
        return http_json(e.status, Json(std::move(root)).dump());
    } catch (const JsonError& e) {
        return http_error(400, "bad_json", e.what());
    } catch (const std::runtime_error& e) {
        return http_error(400, "bad_request", e.what());
    } catch (const std::exception& e) {
        return http_error(500, "internal_error", e.what());
    }
}

} // namespace macha

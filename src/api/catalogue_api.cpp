// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/catalogue_api.hpp"
#include "catalogue/media_information.hpp"
#include "macha_version.hpp"

#include "crypto.hpp"
#include "log.hpp"
#include "json.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <charconv>
#include <cctype>
#include <cstring>
#include <fstream>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace macha {
namespace {
std::string json_escape(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out{"\""};
    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0xf]);
                out.push_back(hex[c & 0xf]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

Bytes body(std::string value) {
    return Bytes(value.begin(), value.end());
}

HttpResponse json(int status, std::string value) {
    return {status, "application/json; charset=utf-8", {}, body(std::move(value))};
}

HttpResponse error(int status, std::string_view code, std::string_view message) {
    return json(status, "{\"error\":" + json_escape(code) + ",\"message\":" +
                            json_escape(message) + "}");
}

// Same shape as error(), plus the engine's reason it could not produce facts;
// the client decides whether to try another node, another source, or give up.
HttpResponse media_error(int status, std::string_view code, std::string_view message,
                         MediaFailure failure) {
    return json(status, "{\"error\":" + json_escape(code) + ",\"message\":" +
                            json_escape(message) + ",\"reason\":" +
                            json_escape(media_failure_name(failure)) + "}");
}

std::string optional_number(const std::optional<int32_t>& value) {
    return value ? std::to_string(*value) : "null";
}

// Signs and verifies artwork capability URLs, so catalogue responses embed an
// already-authorised <img src> without a bearer header. Artwork has no session
// to bound it, so the expiry is explicit and carried beside the signature.
struct ArtworkUrlContext {
    const ClusterKeys& keys;
    std::chrono::milliseconds ttl;
};

std::array<uint8_t, 32> artwork_capability_mac(const ClusterKeys& keys, std::string_view id,
                                               uint64_t expires_unix_ms) {
    std::string material = "macha-artwork-capability-v1";
    material.push_back('\0');
    material.append(id);
    material.push_back('\0');
    material.append(std::to_string(expires_unix_ms));
    return hmac_sha256(keys.auth,
                       {reinterpret_cast<const uint8_t*>(material.data()), material.size()});
}

// The expiry is quantised to a TTL bucket so the URL is byte-identical for
// every request in a bucket: a browser keys its cache on the full URL, so a
// fresh `exp` would defeat caching. Rounding to the bucket after next keeps
// remaining validity between one and two TTLs.
std::string signed_artwork_url(const ArtworkUrlContext& ctx, std::string_view id) {
    const auto now = unix_ms();
    const auto ttl = ctx.ttl.count() > 0 ? static_cast<uint64_t>(ctx.ttl.count()) : uint64_t{0};
    const auto expires = ttl ? (now / ttl + 2) * ttl : now;
    const auto mac = artwork_capability_mac(ctx.keys, id, expires);
    return "/api/v1/catalogue/artwork/" + std::string(id) + "?exp=" + std::to_string(expires) +
          "&sig=" + hex(mac);
}

// Only a valid, unexpired signature grants the exemption; an unsigned request
// to this path still needs the ordinary bearer token when one is configured.
bool artwork_capability_valid(const ClusterKeys& keys, std::string_view id,
                              const std::map<std::string, std::string, std::less<>>& query) {
    auto exp_it = query.find("exp");
    auto sig_it = query.find("sig");
    if (exp_it == query.end() || sig_it == query.end())
        return false;
    uint64_t expires = 0;
    const auto& exp_text = exp_it->second;
    auto [end, ec] = std::from_chars(exp_text.data(), exp_text.data() + exp_text.size(), expires);
    if (ec != std::errc{} || end != exp_text.data() + exp_text.size())
        return false;
    if (unix_ms() >= expires)
        return false;
    auto provided = unhex(sig_it->second);
    if (!provided)
        return false;
    const auto expected = artwork_capability_mac(keys, id, expires);
    return provided->size() == expected.size() &&
          constant_time_equal(*provided, expected);
}

std::string artwork_json(const std::vector<CatalogueArtwork>& artwork,
                         const ArtworkUrlContext& urls) {
    std::string out = "[";
    for (size_t i = 0; i < artwork.size(); ++i) {
        if (i) out += ',';
        const auto& art = artwork[i];
        const auto id_hex = to_string(art.id);
        out += "{\"role\":" + json_escape(art.role) + ",\"id\":" + json_escape(id_hex) +
               ",\"mime_type\":" + json_escape(art.mime_type) + ",\"url\":" +
               json_escape(signed_artwork_url(urls, id_hex)) + "}";
    }
    out += ']';
    return out;
}

// `availability`: complete, partial, unavailable or unknown; for a set,
// `availability_members` counts the items beneath it that have files.
std::string availability_json(const CatalogueItem& item, const ItemAvailabilityTable& table) {
    ItemAvailability entry;
    if (const auto found = table.find(item.id); found != table.end())
        entry = found->second;
    std::string out = ",\"availability\":" + json_escape(availability_name(entry.status));
    out += ",\"availability_members\":";
    if (!entry.members)
        return out + "null";
    return out + "{\"total\":" + std::to_string(entry.members) +
           ",\"complete\":" + std::to_string(entry.complete) +
           ",\"partial\":" + std::to_string(entry.partial) +
           ",\"unavailable\":" + std::to_string(entry.unavailable) +
           ",\"unknown\":" + std::to_string(entry.unknown) + "}";
}

std::string item_json(const CatalogueItem& item, const CatalogueSnapshot& snapshot,
                      const ArtworkUrlContext& urls, const ItemAvailabilityTable& availability) {
    std::string out = "{";
    out += "\"id\":" + json_escape(item.id);
    out += ",\"kind\":" + json_escape(catalogue_kind_name(item.kind));
    out += ",\"title\":" + json_escape(item.title);
    out += ",\"sort_title\":" + json_escape(item.sort_title);
    out += ",\"synopsis\":" + json_escape(item.synopsis);
    out += ",\"parent_id\":" + (item.parent_id ? json_escape(*item.parent_id) : "null");
    out += ",\"year\":" + optional_number(item.year);
    out += ",\"season_number\":" + optional_number(item.season_number);
    out += ",\"episode_number\":" + optional_number(item.episode_number);
    out += ",\"disc_number\":" + optional_number(item.disc_number);
    out += ",\"track_number\":" + optional_number(item.track_number);
    out += ",\"aliases\":[";
    for (size_t i = 0; i < item.aliases.size(); ++i) {
        if (i) out += ',';
        out += json_escape(item.aliases[i]);
    }
    out += "],\"external_ids\":{";
    bool first = true;
    for (const auto& [provider, id] : item.external_ids) {
        if (!first) out += ',';
        first = false;
        out += json_escape(provider) + ":" + json_escape(id);
    }
    out += "},\"media_ids\":[";
    for (size_t i = 0; i < item.media_ids.size(); ++i) {
        if (i) out += ',';
        out += json_escape(item.media_ids[i]);
    }
    out += "],\"artwork\":" + artwork_json(item.artwork, urls);
    out += ",\"effective_artwork\":" + artwork_json(effective_catalogue_artwork(snapshot, item), urls);
    out += availability_json(item, availability);
    out += ",\"revision\":" + std::to_string(item.revision);
    out += ",\"updated_ns\":" + std::to_string(item.updated_ns) + "}";
    return out;
}

std::string items_json(const std::vector<CatalogueItem>& items, const CatalogueSnapshot& snapshot,
                       const ArtworkUrlContext& urls, const ItemAvailabilityTable& availability) {
    std::string out = "{\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ',';
        out += item_json(items[i], snapshot, urls, availability);
    }
    out += "]}";
    return out;
}

Json media_profile_json(std::string_view media_id, const MediaProbeResult& profile,
                        std::optional<uint64_t> size) {
    Json::Array streams;
    for (const auto& stream : profile.streams) {
        Json::Object value{{"index", stream.index},
                           {"type", media_stream_type_name(stream.type)},
                           {"codec", stream.codec},
                           {"profile", stream.profile},
                           {"language", stream.language},
                           {"width", stream.width},
                           {"height", stream.height},
                           {"channels", stream.channels},
                           {"sample_rate", stream.sample_rate},
                           {"bit_depth", stream.bit_depth},
                           {"level", stream.level},
                           {"color_transfer", stream.color_transfer},
                           {"dolby_vision_profile", stream.dolby_vision_profile},
                           {"dolby_vision_compatibility", stream.dolby_vision_compatibility},
                           {"default", stream.default_stream},
                           {"forced", stream.forced},
                           {"bitrate", stream.bitrate},
                           {"attached_picture", stream.attached_picture}};
        streams.emplace_back(std::move(value));
    }
    Json::Object out{{"schema_version", 3},
                     {"media_id", std::string(media_id)},
                     {"format", profile.format},
                     {"size", size ? Json(*size) : Json(nullptr)},
                     {"duration_ms", static_cast<uint64_t>(
                         std::max(0.0, profile.duration_seconds) * 1000.0)},
                     {"bitrate", profile.bitrate},
                     {"streams", Json(std::move(streams))}};
    return Json(std::move(out));
}

std::optional<int32_t> json_i32(const Json& root, std::string_view key) {
    const auto* value = root.find(key);
    if (!value || value->isNull()) return {};
    auto number = value->asInt64();
    if (number < INT32_MIN || number > INT32_MAX)
        throw std::runtime_error("catalogue integer out of range");
    return static_cast<int32_t>(number);
}

// A request body or item the API cannot accept: a 400 with its own code, not a
// catalogue failure. `fields` are stated beside the code.
struct BadItem : std::runtime_error {
    std::string code;
    std::vector<std::pair<std::string, std::string>> fields;
    BadItem(std::string error_code, const std::string& message,
            std::vector<std::pair<std::string, std::string>> extra = {})
        : std::runtime_error(message), code(std::move(error_code)), fields(std::move(extra)) {}
};

Json parse_item_body(std::span<const uint8_t> bytes) {
    try {
        std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        auto root = Json::parse(text);
        if (!root.isObject()) throw BadItem("bad_item", "catalogue item body must be a JSON object");
        return root;
    } catch (const BadItem&) {
        throw;
    } catch (const std::exception& e) {
        throw BadItem("bad_item", e.what());
    }
}

// Sets every field `root` states. For a whole item (PUT) kind and title are
// required; a partial update (PATCH) changes only what is present, and a null
// clears an optional field.
void apply_item_fields(CatalogueItem& item, const Json& root, bool whole) {
    try {
        const auto* kind_value = root.find("kind");
        const auto* title_value = root.find("title");
        if (whole && (!kind_value || !title_value))
            throw BadItem("bad_item", "catalogue item requires kind and title");
        if (kind_value) {
            auto kind = parse_catalogue_kind(kind_value->asString());
            if (!kind) throw BadItem("bad_item", "unknown catalogue kind");
            item.kind = *kind;
        }
        if (title_value) item.title = title_value->asString();
        if (const auto* value = root.find("sort_title")) item.sort_title = value->asString();
        if (const auto* value = root.find("synopsis")) item.synopsis = value->asString();
        if (const auto* value = root.find("parent_id"))
            item.parent_id = value->isNull() ? std::nullopt
                                             : std::optional<std::string>(value->asString());
        const auto number = [&](std::string_view key, std::optional<int32_t>& field) {
            if (root.find(key)) field = json_i32(root, key);
        };
        number("year", item.year);
        number("season_number", item.season_number);
        number("episode_number", item.episode_number);
        number("disc_number", item.disc_number);
        number("track_number", item.track_number);
        if (const auto* aliases = root.find("aliases")) {
            if (!aliases->isArray()) throw BadItem("bad_item", "aliases must be an array");
            item.aliases.clear();
            for (const auto& alias : aliases->asArray()) item.aliases.push_back(alias.asString());
        }
        if (const auto* external = root.find("external_ids")) {
            if (!external->isObject()) throw BadItem("bad_item", "external_ids must be an object");
            item.external_ids.clear();
            for (const auto& [provider, value] : external->asObject())
                item.external_ids[provider] = value.asString();
        }
        if (const auto* media = root.find("media_ids")) {
            if (!media->isArray()) throw BadItem("bad_item", "media_ids must be an array");
            item.media_ids.clear();
            for (const auto& value : media->asArray()) item.media_ids.push_back(value.asString());
        }
        if (const auto* artwork = root.find("artwork")) {
            if (!artwork->isArray()) throw BadItem("bad_item", "artwork must be an array");
            item.artwork.clear();
            for (const auto& value : artwork->asArray()) {
                const auto* id_value = value.find("id");
                const auto* role = value.find("role");
                const auto* mime = value.find("mime_type");
                if (!id_value || !role || !mime)
                    throw BadItem("bad_item", "artwork requires role, id and mime_type");
                auto decoded = unhex(id_value->asString());
                if (!decoded || decoded->size() != 32)
                    throw BadItem("bad_item", "bad artwork object id");
                CatalogueArtwork art;
                art.role = role->asString();
                std::copy(decoded->begin(), decoded->end(), art.id.bytes.begin());
                art.mime_type = mime->asString();
                item.artwork.push_back(std::move(art));
            }
        }
        // A hand edit is locked against the scanner unless it says otherwise.
        bool lock = true;
        if (const auto* value = root.find("lock")) lock = value->asBool();
        if (lock)
            item.external_ids["macha_metadata_locked"] = "1";
        else
            item.external_ids.erase("macha_metadata_locked");
    } catch (const BadItem&) {
        throw;
    } catch (const std::exception& e) {
        throw BadItem("bad_item", e.what());
    }
}

// The kind an item's parent must be, or none for a top-level kind.
std::optional<CatalogueKind> required_parent_kind(CatalogueKind kind) {
    switch (kind) {
    case CatalogueKind::season: return CatalogueKind::show;
    case CatalogueKind::episode: return CatalogueKind::season;
    case CatalogueKind::album: return CatalogueKind::artist;
    case CatalogueKind::track: return CatalogueKind::album;
    default: return std::nullopt;
    }
}

void validate_parent(CatalogueManager& catalogue, const CatalogueItem& item) {
    if (!item.parent_id) return;
    const auto parent = catalogue.get(*item.parent_id);
    if (!parent)
        throw BadItem("parent_not_found", "parent item not found",
                      {{"parent_id", *item.parent_id}});
    const auto wanted = required_parent_kind(item.kind);
    if (!wanted || parent->kind != *wanted)
        throw BadItem("bad_parent_kind", "parent is the wrong kind for this item",
                      {{"kind", std::string(catalogue_kind_name(item.kind))},
                       {"parent_kind", std::string(catalogue_kind_name(parent->kind))}});
}

std::optional<uint64_t> expected_revision(const HttpRequest& request) {
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

std::string url_decode(std::string_view value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = nibble(value[i + 1]), lo = nibble(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return out;
}

} // namespace

std::shared_ptr<const ItemAvailabilityTable>
CatalogueApi::item_availability(const std::shared_ptr<const CatalogueSnapshot>& snapshot) {
    return item_availability_.table(snapshot, availability_ ? availability_() : nullptr);
}

HttpResponse CatalogueApi::handle(const HttpRequest& request) {
    try {
        if (request.method == "GET" && request.path == "/api/v1/catalogue/status") {
            auto status = catalogue_.status();
            std::string out = "{\"server_version\":" + json_escape(kServerVersion) + ",\"enabled\":true,\"ready\":" + std::string(status.ready ? "true" : "false");
            out += ",\"metadata_generation\":" + std::to_string(status.metadata_generation);
            out += ",\"known_metadata_generation\":" + std::to_string(status.known_metadata_generation);
            out += ",\"root\":" + (status.root ? json_escape(to_string(*status.root)) : "null");
            out += ",\"items\":" + std::to_string(status.items);
            out += ",\"artwork_objects\":" + std::to_string(status.artwork_objects);
            out += ",\"local_artwork_objects\":" + std::to_string(status.local_artwork_objects);
            out += ",\"last_sync_unix_ms\":" + std::to_string(status.last_sync_unix_ms);
            out += ",\"error_code\":" + (status.error_code.empty() ? "null" : json_escape(status.error_code));
            out += ",\"error\":" + (status.error.empty() ? "null" : json_escape(status.error)) + "}";
            return json(200, std::move(out));
        }

        if (request.method == "GET" && request.path == "/api/v1/catalogue/hints") {
            Json::Array values;
            for (const auto& hint : hints_.list()) {
                Json::Object item;
                item["id"] = hint.id;
                item["path"] = hint.path;
                item["priority"] = static_cast<int64_t>(hint.priority);
                item["state"] = catalogue_hint_state_name(hint.state);
                item["attempts"] = static_cast<uint64_t>(hint.attempts);
                item["failures"] = static_cast<uint64_t>(hint.failures);
                item["candidate_cursor"] = static_cast<uint64_t>(hint.candidate_cursor);
                item["ready_after_unix_ms"] = hint.ready_after_unix_ms;
                item["provider"] = hint.provider.empty() ? Json(nullptr) : Json(hint.provider);
                item["media_id"] = hint.media_id.empty() ? Json(nullptr) : Json(hint.media_id);
                item["result"] = hint.result.empty() ? Json(nullptr) : Json(hint.result);
                item["error_code"] = hint.error_code.empty() ? Json(nullptr) : Json(hint.error_code);
                item["error"] = hint.error.empty() ? Json(nullptr) : Json(hint.error);
                Json::Array catalogue_ids;
                for (const auto& id : hint.catalogue_item_ids) catalogue_ids.emplace_back(id);
                item["catalogue_item_ids"] = std::move(catalogue_ids);
                Json::Array origins;
                for (const auto& origin : hint.origins) {
                    Json::Object value;
                    value["source"] = origin.source;
                    value["source_ref"] = origin.source_ref;
                    value["priority"] = static_cast<int64_t>(origin.priority);
                    origins.emplace_back(std::move(value));
                }
                item["origins"] = std::move(origins);
                item["created_unix_ms"] = hint.created_unix_ms;
                item["updated_unix_ms"] = hint.updated_unix_ms;
                values.emplace_back(std::move(item));
            }
            Json::Object out;
            out["hints"] = std::move(values);
            return json(200, Json(std::move(out)).dump());
        }

        if (request.method == "GET" && request.path == "/api/v1/catalogue/items") {
            std::optional<CatalogueKind> kind;
            if (auto it = request.query.find("type"); it != request.query.end() && !it->second.empty()) {
                kind = parse_catalogue_kind(it->second);
                if (!kind) return error(400, "bad_type", "unknown catalogue type");
            }
            std::optional<std::string_view> parent;
            if (auto it = request.query.find("parent"); it != request.query.end()) parent = it->second;
            auto snapshot = catalogue_.snapshot_view(
                WorkContext(FrameType::control, {}, nullptr, "GET /api/v1/catalogue/items"));
            return json(200, items_json(catalogue_.list(kind, parent), *snapshot,
                                        {catalogue_.cluster_keys(), artwork_capability_ttl_},
                                        *item_availability(snapshot)));
        }

        if (request.method == "GET" && request.path == "/api/v1/catalogue/search") {
            auto q = request.query.find("q");
            if (q == request.query.end()) return error(400, "missing_query", "q is required");
            size_t limit = 50;
            if (auto it = request.query.find("limit"); it != request.query.end() && !it->second.empty()) {
                auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), limit);
                if (ec != std::errc{} || end != it->second.data() + it->second.size() || limit > 1000)
                    return error(400, "bad_limit", "limit must be 0..1000");
            }
            // `kind` may repeat; absent means every kind. `parent` keeps only
            // that item's children. Both filter before `limit`.
            std::vector<std::string> kind_names;
            if (auto all = request.query_all.find("kind"); all != request.query_all.end())
                kind_names = all->second;
            else if (auto one = request.query.find("kind"); one != request.query.end())
                kind_names = {one->second};
            std::set<CatalogueKind> kinds;
            for (const auto& name : kind_names) {
                auto kind = parse_catalogue_kind(name);
                if (!kind) return error(400, "bad_kind", "unknown catalogue kind: " + name);
                kinds.insert(*kind);
            }
            std::optional<std::string> parent;
            if (auto it = request.query.find("parent"); it != request.query.end()) parent = it->second;
            auto keep = [&](const CatalogueItem& item) {
                if (!kinds.empty() && !kinds.contains(item.kind)) return false;
                if (parent && item.parent_id != parent) return false;
                return true;
            };
            auto snapshot = catalogue_.snapshot_view(
                WorkContext(FrameType::control, {}, nullptr, "GET /api/v1/catalogue/search"));
            return json(200, items_json(catalogue_.search(q->second, limit, keep), *snapshot,
                                        {catalogue_.cluster_keys(), artwork_capability_ttl_},
                                        *item_availability(snapshot)));
        }

        constexpr std::string_view media_prefix = "/api/v1/catalogue/media/";
        constexpr std::string_view keyframes_suffix = "/keyframes";
        if (request.method == "GET" && request.path.starts_with(media_prefix) &&
            request.path.ends_with(keyframes_suffix)) {
            auto encoded = std::string_view(request.path).substr(media_prefix.size());
            encoded.remove_suffix(keyframes_suffix.size());
            auto media_id = url_decode(encoded);
            if (!media_id.starts_with("macha:"))
                return error(400, "bad_media_id", "immutable macha media ID required");
            std::optional<Bytes> index;
            try {
                if (keyframe_index_) index = keyframe_index_(media_id);
            } catch (const KeyframeIndexUnsupported& e) {
                return error(422, "keyframes_not_supported", e.what());
            } catch (const MediaError& e) {
                return media_error(422, "keyframes_failed", e.what(), e.failure());
            }
            if (!index) return error(404, "not_found", "media is not available on this node");
            HttpResponse response{200, "application/json; charset=utf-8", {}, std::move(*index)};
            // A media id names its bytes, so its index never changes.
            response.headers["Cache-Control"] = "private, max-age=31536000, immutable";
            response.headers["ETag"] = json_escape(media_id);
            return response;
        }
        constexpr std::string_view profile_suffix = "/profile";
        if (request.method == "GET" && request.path.starts_with(media_prefix) &&
            request.path.ends_with(profile_suffix)) {
            auto encoded = std::string_view(request.path).substr(media_prefix.size());
            encoded.remove_suffix(profile_suffix.size());
            auto media_id = url_decode(encoded);
            if (!media_id.starts_with("macha:"))
                return error(400, "bad_media_id", "immutable macha media ID required");
            auto profile = catalogue_.media_profile(media_id);
            if (!profile && resolve_media_profile_) {
                // No stored profile: produce and persist one now on the foreground path a
                // session create uses; background profiling yields to it.
                try {
                    profile = resolve_media_profile_(media_id);
                } catch (const MediaEngineUnavailable& e) {
                    return error(503, "media_engine_unavailable", e.what());
                } catch (const MediaError& e) {
                    // Report the failure kind: an unreadable source may be fine elsewhere; an
                    // unparseable one is unparseable everywhere.
                    return media_error(422, "profile_failed", e.what(), e.failure());
                } catch (const std::exception& e) {
                    return error(422, "profile_failed", e.what());
                }
            }
            if (!profile) {
                const auto accepted = request_media_profiles_
                                          ? request_media_profiles_({media_id})
                                          : 0;
                if (!accepted)
                    return error(404, "not_found",
                                 "media is not available on this node");
                Json::Object pending{{"status", "pending"}, {"media_id", media_id}};
                auto response = json(202, Json(std::move(pending)).dump());
                response.headers["Retry-After"] = "1";
                response.headers["Location"] = request.path;
                return response;
            }
            // A media id names its bytes, so its size never changes; a size
            // this node cannot find yet is not cached as an answer.
            const auto size = media_size_ ? media_size_(media_id) : std::nullopt;
            auto response = json(200, media_profile_json(media_id, *profile, size).dump());
            response.headers["Cache-Control"] = size ? "private, max-age=31536000, immutable"
                                                     : "private, no-cache";
            response.headers["ETag"] = json_escape(media_id);
            return response;
        }

        constexpr std::string_view item_prefix = "/api/v1/catalogue/items/";
        if (request.path.starts_with(item_prefix)) {
            auto rest = std::string_view(request.path).substr(item_prefix.size());
            auto metadata_suffix = rest.rfind("/metadata");
            if (metadata_suffix != std::string_view::npos && metadata_suffix + 9 == rest.size() &&
                request.method == "DELETE") {
                auto id = url_decode(rest.substr(0, metadata_suffix));
                const auto revision = expected_revision(request);
                // A known-current negative answers 404 without a replica repair. If the local
                // catalogue cannot prove it from current immutable state, fall through to the
                // strong mutation path.
                if (catalogue_.definitely_absent(id))
                    return error(404, "not_found", "catalogue item not found");

                auto cleared = catalogue_.clear_metadata_with_media(id, revision);
                if (!cleared.removed_items)
                    return error(404, "not_found", "catalogue item not found");
                if (request_media_rescan_ && !cleared.media_ids.empty()) {
                    try {
                        request_media_rescan_(cleared.media_ids);
                    } catch (const std::exception& e) {
                        // The metadata mutation is committed; targeted rematching is recoverable
                        // background work, with the scanner as fallback.
                        Log::warn("catalogue metadata clear rematch enqueue failed: " +
                                  std::string(e.what()));
                    }
                }
                return {204, "application/json; charset=utf-8", {}, {}};
            }

            auto artwork_suffix = rest.rfind("/artwork");
            if (artwork_suffix != std::string_view::npos && artwork_suffix + 8 == rest.size() &&
                request.method == "POST") {
                auto id = url_decode(rest.substr(0, artwork_suffix));
                auto role = request.query.find("role");
                auto mime = request.query.find("mime");
                if (role == request.query.end() || role->second.empty() || mime == request.query.end() || mime->second.empty())
                    return error(400, "bad_artwork", "role and mime are required");
                auto art = catalogue_.put_artwork(id, role->second, mime->second, request.body,
                                                  expected_revision(request));
                auto updated = catalogue_.get(id);
                auto response = json(201, "{\"role\":" + json_escape(art.role) +
                                             ",\"id\":" + json_escape(to_string(art.id)) +
                                             ",\"mime_type\":" + json_escape(art.mime_type) + "}");
                if (updated) response.headers["ETag"] = "\"rev-" + std::to_string(updated->revision) + "\"";
                return response;
            }

            auto id = url_decode(rest);
            if (request.method == "GET") {
                auto item = catalogue_.get(id);
                if (!item) return error(404, "not_found", "catalogue item not found");
                auto snapshot = catalogue_.snapshot_view(
                WorkContext(FrameType::control, {}, nullptr, "/api/v1/catalogue/items/{id}"));
                auto response = json(200, item_json(*item, *snapshot,
                                                    {catalogue_.cluster_keys(), artwork_capability_ttl_},
                                                    *item_availability(snapshot)));
                response.headers["ETag"] = "\"rev-" + std::to_string(item->revision) + "\"";
                return response;
            }
            if (request.method == "PUT" || request.method == "PATCH") {
                auto existing = catalogue_.get(id);
                if (request.method == "PATCH" && !existing)
                    return error(404, "not_found", "catalogue item not found");
                const auto root = parse_item_body(request.body);
                // A PUT replaces descriptive fields, but files and artwork change only when
                // named: leaving media_ids out must not unbind every file.
                CatalogueItem item;
                if (request.method == "PATCH") {
                    item = *existing;
                } else {
                    item.id = id;
                    if (existing) {
                        item.media_ids = existing->media_ids;
                        item.artwork = existing->artwork;
                    }
                }
                apply_item_fields(item, root, request.method == "PUT");
                validate_parent(catalogue_, item);
                auto saved = catalogue_.upsert(std::move(item), expected_revision(request));
                auto snapshot = catalogue_.snapshot_view(
                WorkContext(FrameType::control, {}, nullptr, "/api/v1/catalogue/items/{id}"));
                auto response = json(existing ? 200 : 201, item_json(saved, *snapshot,
                                                    {catalogue_.cluster_keys(), artwork_capability_ttl_},
                                                    *item_availability(snapshot)));
                response.headers["ETag"] = "\"rev-" + std::to_string(saved.revision) + "\"";
                return response;
            }
            if (request.method == "DELETE") {
                if (!catalogue_.erase(id, expected_revision(request)))
                    return error(404, "not_found", "catalogue item not found");
                return {204, "application/json; charset=utf-8", {}, {}};
            }
        }

        constexpr std::string_view artwork_prefix = "/api/v1/catalogue/artwork/";
        if (request.method == "GET" && request.path.starts_with(artwork_prefix)) {
            auto text = url_decode(std::string_view(request.path).substr(artwork_prefix.size()));
            auto decoded = unhex(text);
            if (!decoded || decoded->size() != 32) return error(400, "bad_id", "bad artwork object id");
            ObjectId id;
            std::copy(decoded->begin(), decoded->end(), id.bytes.begin());
            // The id is a content hash, so the bytes are immutable and the id is the
            // entity tag: a revalidating browser is answered before any artwork read.
            const auto tag = "\"" + to_string(id) + "\"";
            const auto max_age =
                std::chrono::duration_cast<std::chrono::seconds>(artwork_capability_ttl_).count();
            std::map<std::string, std::string, std::less<>> headers{
                {"Cache-Control", "public, max-age=" + std::to_string(max_age) + ", immutable"},
                {"ETag", tag},
                // Without it the browser zeroes every timing for a cross-origin poster.
                {"Timing-Allow-Origin", "*"}};
            if (auto it = request.headers.find("if-none-match");
                it != request.headers.end() && it->second == tag)
                return {304, "", std::move(headers), {}};
            auto artwork = catalogue_.artwork(id);
            if (!artwork) return error(404, "not_found", "artwork not found");
            return {200, std::move(artwork->mime_type), std::move(headers),
                   std::move(artwork->bytes)};
        }

        return error(404, "not_found", "endpoint not found");
    } catch (const CatalogueConflict& e) {
        return error(409, "conflict", e.what());
    } catch (const BadItem& e) {
        std::string body = "{\"error\":" + json_escape(e.code) + ",\"message\":" + json_escape(e.what());
        for (const auto& [name, value] : e.fields)
            body += "," + json_escape(name) + ":" + json_escape(value);
        return json(400, body + "}");
    } catch (const std::exception& e) {
        return error(503, "catalogue_unavailable", e.what());
    }
}

bool CatalogueApi::capability_request(const HttpRequest& request) const {
    constexpr std::string_view artwork_prefix = "/api/v1/catalogue/artwork/";
    if (request.method != "GET" || !request.path.starts_with(artwork_prefix))
        return false;
    const auto id = url_decode(std::string_view(request.path).substr(artwork_prefix.size()));
    return artwork_capability_valid(catalogue_.cluster_keys(), id, request.query);
}

} // namespace macha
